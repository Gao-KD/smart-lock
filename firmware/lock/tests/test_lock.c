#include <string.h>

#include "hal_host.h"
#include "lock_app.h"
#include "lock_mac.h"
#include "unit.h"

static lock_app_t app;

static void setup(void)
{
    hal_host_reset();
    lock_app_init(&app);
}

static size_t build(uint8_t *buf, size_t cap, uint8_t cmd, uint32_t msg_id, uint32_t expires,
                    slp_app_writer_t *w)
{
    slp_app_hdr_t h = { .cmd = cmd, .msg_id = msg_id, .expires_at = expires };
    slp_app_begin(w, buf, cap, &h);
    return w->len;
}

static uint8_t result_of(const uint8_t *rsp, size_t len, uint32_t expect_msg_id)
{
    slp_app_reader_t r;
    slp_app_hdr_t h;
    const uint8_t *v;
    uint8_t l;
    if (slp_app_parse(&r, rsp, len, &h) != SLP_OK || h.cmd != SLP_RSP_RESULT ||
        h.msg_id != expect_msg_id || slp_app_find(&r, SLP_TLV_RESULT, &v, &l) != 1) {
        return 0xFF;
    }
    return v[0];
}

static uint8_t send_simple(uint8_t cmd, uint32_t msg_id, uint32_t expires)
{
    uint8_t req[64], rsp[64];
    slp_app_writer_t w;
    build(req, sizeof(req), cmd, msg_id, expires, &w);
    size_t n = lock_app_handle_command(&app, req, w.len, rsp, sizeof(rsp));
    return result_of(rsp, n, msg_id);
}

static void add_card(const uint8_t *uid, uint8_t len, uint32_t from, uint32_t to, uint32_t msg_id)
{
    uint8_t req[64], rsp[64];
    slp_app_writer_t w;
    build(req, sizeof(req), SLP_CMD_CRED_ADD, msg_id, 0, &w);
    slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, SLP_CRED_CARD_UID);
    slp_app_put(&w, SLP_TLV_CRED_ID, uid, len);
    if (from) slp_app_put_u32(&w, SLP_TLV_VALID_FROM, from);
    if (to) slp_app_put_u32(&w, SLP_TLV_VALID_TO, to);
    size_t n = lock_app_handle_command(&app, req, w.len, rsp, sizeof(rsp));
    CHECK_EQ(result_of(rsp, n, msg_id), SLP_RESULT_OK);
}

static void test_remote_unlock_and_auto_relock(void)
{
    setup();
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 1, hal_unix_time() + 30), SLP_RESULT_OK);
    CHECK_EQ(app.core.state, LOCK_ST_UNLOCKED);
    CHECK(hal_host.clutch_engaged);
    hal_host_advance(4999);
    lock_core_tick(&app.core);
    CHECK_EQ(app.core.state, LOCK_ST_UNLOCKED);
    hal_host_advance(1);
    lock_core_tick(&app.core);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED);
    CHECK(!hal_host.clutch_engaged);
}

static void test_expired_and_duplicate_commands(void)
{
    setup();
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 7, hal_unix_time() - 1), SLP_RESULT_EXPIRED);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED);
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 8, 0), SLP_RESULT_OK);
    int calls = hal_host.clutch_calls;
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 8, 0), SLP_RESULT_DUPLICATE);
    CHECK_EQ(hal_host.clutch_calls, calls); /* not executed twice */
}

static void test_campus_card_uid_access(void)
{
    setup();
    const uint8_t uid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t other[4] = { 0x01, 0x02, 0x03, 0x04 };
    uint32_t now = hal_unix_time();
    add_card(uid, 4, now - 10, now + 3600, 100);

    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, other, 4), SLP_RESULT_DENIED);
    CHECK_EQ(hal_host.last_ind, HAL_IND_DENIED);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    CHECK_EQ(app.core.state, LOCK_ST_UNLOCKED);

    /* Outside the validity window (e.g. after graduation date). */
    hal_set_unix_time(now + 7200);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_DENIED);
}

static void test_lockdown_blocks_users_not_remote(void)
{
    setup();
    const uint8_t uid[7] = { 1, 2, 3, 4, 5, 6, 7 };
    add_card(uid, 7, 0, 0, 200);
    CHECK_EQ(send_simple(SLP_CMD_LOCKDOWN, 201, 0), SLP_RESULT_OK);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 7), SLP_RESULT_DENIED);
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 202, 0), SLP_RESULT_OK);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKDOWN); /* one-off open, still locked down */
    CHECK(hal_host.clutch_engaged);
    hal_host_advance(6000);
    lock_core_tick(&app.core);
    CHECK(!hal_host.clutch_engaged);
    CHECK_EQ(send_simple(SLP_CMD_RELEASE, 203, 0), SLP_RESULT_OK);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 7), SLP_RESULT_OK);
}

static void test_emergency_holds_open(void)
{
    setup();
    CHECK_EQ(send_simple(SLP_CMD_EMERGENCY, 300, 0), SLP_RESULT_OK);
    hal_host_advance(600000);
    lock_core_tick(&app.core);
    CHECK_EQ(app.core.state, LOCK_ST_EMERGENCY);
    CHECK(hal_host.clutch_engaged);
    CHECK_EQ(send_simple(SLP_CMD_RELEASE, 301, 0), SLP_RESULT_OK);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED);
}

static void test_clutch_fault(void)
{
    setup();
    hal_host.clutch_fail_next = 1; /* one failure is retried */
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 400, 0), SLP_RESULT_OK);
    setup();
    hal_host.clutch_fail_next = 2;
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 401, 0), SLP_RESULT_HW_FAULT);
    CHECK_EQ(app.core.state, LOCK_ST_FAULT);
}

static void test_mac_wakeup_and_filtering(void)
{
    lock_mac_t m;
    lock_mac_init(&m, 0x0101, 0x0042, 3, 25, 1000, 20);
    CHECK_EQ(lock_mac_next_wakeup(&m, 5000), 6000u); /* unsynced: once per period */

    lock_mac_on_beacon(&m, 10000, 0);
    /* slot 25 starts at 10500; wake 2 ms early. */
    CHECK_EQ(lock_mac_next_wakeup(&m, 10001), 10498u);
    CHECK_EQ(lock_mac_next_wakeup(&m, 10499), 11498u);

    lock_mac_check_sync(&m, 10000 + LOCK_MAC_SYNC_TIMEOUT_MS + 1);
    CHECK_EQ(m.sync, LOCK_MAC_UNSYNCED);

    slp_frame_t f = { .type = SLP_FT_DATA, .security = true, .pan_id = 0x0101,
                      .dst_mode = SLP_ADDR_SHORT, .dst = 0x0042, .frame_counter = 10 };
    CHECK(lock_mac_addressed_to_me(&m, &f));
    f.dst = 0x0043;
    CHECK(!lock_mac_addressed_to_me(&m, &f));
    f.dst = SLP_SHORT_BROADCAST;
    CHECK(lock_mac_addressed_to_me(&m, &f));
    m.groups[m.group_count++] = 0xF010;
    f.dst = 0xF010;
    CHECK(lock_mac_addressed_to_me(&m, &f));
    f.pan_id = 0x0202;
    CHECK(!lock_mac_addressed_to_me(&m, &f));

    f.pan_id = 0x0101;
    f.dst = 0x0042;
    CHECK_EQ(lock_mac_replay_check(&m, &f), SLP_OK);
    CHECK_EQ(lock_mac_replay_check(&m, &f), SLP_ERR_REPLAY);
    /* Group counters are tracked separately. */
    f.dst = SLP_SHORT_BROADCAST;
    f.frame_counter = 3;
    CHECK_EQ(lock_mac_replay_check(&m, &f), SLP_OK);
}

int main(void)
{
    printf("lock firmware tests\n");
    RUN(test_remote_unlock_and_auto_relock);
    RUN(test_expired_and_duplicate_commands);
    RUN(test_campus_card_uid_access);
    RUN(test_lockdown_blocks_users_not_remote);
    RUN(test_emergency_holds_open);
    RUN(test_clutch_fault);
    RUN(test_mac_wakeup_and_filtering);
    UNIT_DONE();
}
