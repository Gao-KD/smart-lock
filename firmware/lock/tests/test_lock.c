#include <string.h>

#include "hal_host.h"
#include "lock_app.h"
#include "lock_mac.h"
#include "offline_pwd.h"
#include "slp/crypto_host.h"
#include "slp/secure.h"
#include "unit.h"

static lock_app_t app;
static const uint8_t test_app_key[16] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                          0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F };

static void setup(void)
{
    hal_host_reset();
    slp_host_key_set(HAL_HOST_APP_KEY, test_app_key);
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
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_EXPIRED);
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

static void test_access_events_queued(void)
{
    setup();
    const uint8_t uid[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
    const uint8_t bad[4] = { 1, 2, 3, 4 };
    add_card(uid, 4, 0, 0, 1);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, bad, 4), SLP_RESULT_DENIED);

    uint8_t buf[64];
    slp_app_reader_t r;
    slp_app_hdr_t h;
    const uint8_t *v;
    uint8_t l;
    size_t n = lock_app_pop_event(&app, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_EQ(slp_app_parse(&r, buf, n, &h), SLP_OK);
    CHECK_EQ(h.cmd, SLP_EVT_ACCESS);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_RESULT, &v, &l), 1);
    CHECK_EQ(v[0], SLP_RESULT_OK);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_CRED_ID, &v, &l), 1);
    CHECK_EQ(l, 4);
    CHECK(memcmp(v, uid, 4) == 0);

    n = lock_app_pop_event(&app, buf, sizeof(buf));
    CHECK_EQ(slp_app_parse(&r, buf, n, &h), SLP_OK);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_RESULT, &v, &l), 1);
    CHECK_EQ(v[0], SLP_RESULT_DENIED);
    CHECK_EQ(lock_app_pop_event(&app, buf, sizeof(buf)), 0u);
}

static void test_battery_levels_and_hysteresis(void)
{
    setup();
    uint8_t buf[64];
    slp_app_reader_t r;
    slp_app_hdr_t h;
    const uint8_t *v;
    uint8_t l;

    hal_host.battery_mv = 5800;
    CHECK_EQ(lock_app_battery_check(&app), SLP_BATT_OK);
    CHECK_EQ(lock_app_pop_event(&app, buf, sizeof(buf)), 0u);

    hal_host.battery_mv = 4300;
    CHECK_EQ(lock_app_battery_check(&app), SLP_BATT_LOW);
    size_t n = lock_app_pop_event(&app, buf, sizeof(buf));
    CHECK_EQ(slp_app_parse(&r, buf, n, &h), SLP_OK);
    CHECK_EQ(h.cmd, SLP_EVT_BATTERY);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_BATT_LEVEL, &v, &l), 1);
    CHECK_EQ(v[0], SLP_BATT_LOW);

    /* Small recovery (motor load gone) inside the hysteresis: stays LOW. */
    hal_host.battery_mv = 4450;
    CHECK_EQ(lock_app_battery_check(&app), SLP_BATT_LOW);
    CHECK_EQ(lock_app_pop_event(&app, buf, sizeof(buf)), 0u);

    hal_host.battery_mv = 3900;
    CHECK_EQ(lock_app_battery_check(&app), SLP_BATT_CRITICAL);
    CHECK(lock_app_pop_event(&app, buf, sizeof(buf)) > 0);

    /* New batteries. */
    hal_host.battery_mv = 6000;
    CHECK_EQ(lock_app_battery_check(&app), SLP_BATT_OK);
    CHECK(lock_app_pop_event(&app, buf, sizeof(buf)) > 0);

    /* Access with a low battery gives the low-battery indication. */
    const uint8_t uid[4] = { 5, 5, 5, 5 };
    add_card(uid, 4, 0, 0, 77);
    hal_host.battery_mv = 4200;
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    CHECK_EQ(hal_host.last_ind, HAL_IND_LOW_BATT);

    n = lock_app_heartbeat(&app, buf, sizeof(buf), SLP_HB_PERIODIC);
    CHECK_EQ(slp_app_parse(&r, buf, n, &h), SLP_OK);
    CHECK_EQ(h.cmd, SLP_EVT_HEARTBEAT);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_CRED_COUNT, &v, &l), 1);
    CHECK_EQ(slp_le16(v), 1);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_LOCK_STATE, &v, &l), 1);
    CHECK_EQ(v[0], LOCK_ST_UNLOCKED);
}

static void test_event_queue_overflow_keeps_newest(void)
{
    setup();
    const uint8_t bad[4] = { 9, 9, 9, 9 };
    app.cfg.wrong_limit = 0; /* no lockout for this test */
    for (int i = 0; i < 20; i++) {
        lock_app_access(&app, SLP_CRED_CARD_UID, bad, 4);
    }
    CHECK_EQ(app.evq_count, LOCK_EVT_QUEUE);
    CHECK_EQ(app.evt_dropped, 4u);
    CHECK_EQ(app.log_seq, 20u); /* the log ring still has all of them */
}

/* ---- helpers for the feature tests ---- */

static uint8_t send_cmd(slp_app_writer_t *w, uint8_t *req, uint16_t *count)
{
    uint8_t rsp[96];
    slp_app_reader_t r;
    slp_app_hdr_t h, rh;
    const uint8_t *v;
    uint8_t l;
    slp_app_parse(&r, req, w->len, &h);
    size_t n = lock_app_handle_command(&app, req, w->len, rsp, sizeof(rsp));
    if (count && slp_app_parse(&r, rsp, n, &rh) == SLP_OK && slp_app_find(&r, SLP_TLV_COUNT, &v, &l) == 1) {
        *count = slp_le16(v);
    }
    return result_of(rsp, n, h.msg_id);
}

static uint32_t next_id = 5000;

static uint8_t add_password(const char *pin, uint32_t user, uint16_t uses, uint8_t flags,
                            const uint8_t *sched)
{
    uint8_t req[96];
    slp_app_writer_t w;
    build(req, sizeof(req), SLP_CMD_CRED_ADD, ++next_id, 0, &w);
    slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, SLP_CRED_PASSWORD);
    slp_app_put(&w, SLP_TLV_CRED_ID, pin, (uint8_t)strlen(pin));
    slp_app_put_u32(&w, SLP_TLV_USER_ID, user);
    if (uses) slp_app_put_u16(&w, SLP_TLV_USES, uses);
    if (flags) slp_app_put_u8(&w, SLP_TLV_CRED_FLAGS, flags);
    if (sched) slp_app_put(&w, SLP_TLV_SCHEDULE, sched, 5);
    return send_cmd(&w, req, NULL);
}

static slp_result_t keypad(const char *digits)
{
    return lock_app_keypad(&app, (const uint8_t *)digits, (uint8_t)strlen(digits));
}

static void relock(void)
{
    hal_host_advance(10000);
    lock_app_tick(&app);
}

/* Pops every queued event; returns how many had the given type, and the
 * last ALARM value seen. */
static int drain(uint8_t type, uint8_t *last_alarm)
{
    uint8_t buf[96];
    size_t n;
    int k = 0;
    while ((n = lock_app_pop_event(&app, buf, sizeof(buf))) > 0) {
        slp_app_reader_t r;
        slp_app_hdr_t h;
        const uint8_t *v;
        uint8_t l;
        slp_app_parse(&r, buf, n, &h);
        if (h.cmd == type) {
            k++;
            if (last_alarm && slp_app_find(&r, SLP_TLV_ALARM, &v, &l) == 1) *last_alarm = v[0];
        }
    }
    return k;
}

static void test_passwords_decoy_one_time_schedule(void)
{
    setup();
    CHECK_EQ(add_password("123", 1, 0, 0, NULL), SLP_RESULT_BAD_PARAM); /* too short */
    CHECK_EQ(add_password("246810", 1, 0, 0, NULL), SLP_RESULT_OK);
    CHECK_EQ(keypad("246810"), SLP_RESULT_OK);
    relock();
    /* Decoy digits around the password hide it from onlookers. */
    CHECK_EQ(keypad("9924681077"), SLP_RESULT_OK);
    relock();
    CHECK_EQ(keypad("2468"), SLP_RESULT_DENIED);

    /* One-time visitor password: works once, then gone. */
    CHECK_EQ(add_password("555000", 2, 1, 0, NULL), SLP_RESULT_OK);
    uint16_t before = app.creds.count;
    CHECK_EQ(keypad("555000"), SLP_RESULT_OK);
    relock();
    CHECK_EQ(app.creds.count, before - 1);
    CHECK_EQ(keypad("555000"), SLP_RESULT_DENIED);

    /* Weekly schedule: cleaner on Monday..Friday 08:00-10:00 local time.
     * hal_host starts at unix 1800000000 = Fri 2027-01-15 08:00 UTC
     * = 16:00 Beijing (Friday). */
    const uint8_t sched[5] = { 0x1F, (8 * 60) & 0xFF, (8 * 60) >> 8, (10 * 60) & 0xFF, (10 * 60) >> 8 };
    CHECK_EQ(add_password("808080", 3, 0, 0, sched), SLP_RESULT_OK);
    CHECK_EQ(lock_app_local_min_of_week(&app, hal_unix_time()), 4 * 1440 + 16 * 60);
    CHECK_EQ(keypad("808080"), SLP_RESULT_SCHEDULE);
    hal_set_unix_time(1800000000u + 16 * 3600 + 30 * 60); /* Sat 08:30 Beijing: weekend */
    CHECK_EQ(keypad("808080"), SLP_RESULT_SCHEDULE);
    hal_set_unix_time(1800000000u + (2 * 24 + 16) * 3600 + 30 * 60); /* Mon 08:30 */
    CHECK_EQ(keypad("808080"), SLP_RESULT_OK);
}

static void test_wrong_attempts_lockout(void)
{
    setup();
    const uint8_t uid[4] = { 1, 1, 1, 1 }, bad[4] = { 6, 6, 6, 6 };
    add_card(uid, 4, 0, 0, 10);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, bad, 4), SLP_RESULT_DENIED);
    }
    /* A success resets the counter. */
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    relock();
    for (int i = 0; i < 5; i++) {
        lock_app_access(&app, SLP_CRED_CARD_UID, bad, 4);
    }
    uint8_t alarm = 0;
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 1);
    CHECK_EQ(alarm, SLP_ALARM_WRONG_ATTEMPTS);
    CHECK(lock_app_flags(&app) & SLP_LF_LOCKED_OUT);
    /* Even the right card is refused during the lockout... */
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_LOCKED_OUT);
    /* ...but the platform can still open remotely. */
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 77, 0), SLP_RESULT_OK);
    hal_host_advance(180000);
    lock_app_tick(&app);
    CHECK(!(lock_app_flags(&app) & SLP_LF_LOCKED_OUT));
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
}

static void test_duress_freeze_and_move_out(void)
{
    setup();
    CHECK_EQ(add_password("13579246", 42, 0, SLP_CRED_F_DURESS, NULL), SLP_RESULT_OK);
    const uint8_t uid[4] = { 4, 2, 4, 2 };
    uint8_t req[96];
    slp_app_writer_t w;
    build(req, sizeof(req), SLP_CMD_CRED_ADD, ++next_id, 0, &w);
    slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, SLP_CRED_CARD_UID);
    slp_app_put(&w, SLP_TLV_CRED_ID, uid, 4);
    slp_app_put_u32(&w, SLP_TLV_USER_ID, 42);
    CHECK_EQ(send_cmd(&w, req, NULL), SLP_RESULT_OK);
    drain(0, NULL);

    /* Duress password: opens normally, silent alarm to the platform. */
    CHECK_EQ(keypad("13579246"), SLP_RESULT_OK);
    uint8_t alarm = 0;
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 1);
    CHECK_EQ(alarm, SLP_ALARM_DURESS);
    relock();

    /* Rent arrears: freeze the member, every credential stops working. */
    uint16_t count = 0;
    build(req, sizeof(req), SLP_CMD_USER_FREEZE, ++next_id, 0, &w);
    slp_app_put_u32(&w, SLP_TLV_USER_ID, 42);
    slp_app_put_u8(&w, SLP_TLV_FREEZE, 1);
    CHECK_EQ(send_cmd(&w, req, &count), SLP_RESULT_OK);
    CHECK_EQ(count, 2);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_FROZEN);
    build(req, sizeof(req), SLP_CMD_USER_FREEZE, ++next_id, 0, &w);
    slp_app_put_u32(&w, SLP_TLV_USER_ID, 42);
    slp_app_put_u8(&w, SLP_TLV_FREEZE, 0);
    CHECK_EQ(send_cmd(&w, req, &count), SLP_RESULT_OK);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    relock();

    /* Move-out: one command removes all of the member's credentials. */
    build(req, sizeof(req), SLP_CMD_USER_DEL, ++next_id, 0, &w);
    slp_app_put_u32(&w, SLP_TLV_USER_ID, 42);
    CHECK_EQ(send_cmd(&w, req, &count), SLP_RESULT_OK);
    CHECK_EQ(count, 2);
    CHECK_EQ(app.creds.count, 0);
}

static void test_double_lock_door_sensor_and_alarms(void)
{
    setup();
    const uint8_t uid[4] = { 7, 7, 7, 7 };
    add_card(uid, 4, 0, 0, 11);
    lock_app_double_lock(&app, true);
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_DOUBLE_LOCKED);
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 90, 0), SLP_RESULT_DOUBLE_LOCKED);
    CHECK_EQ(app.wrong_count, 0); /* not a wrong attempt */
    lock_app_double_lock(&app, false);
    CHECK_EQ(drain(SLP_EVT_DOOR, NULL), 2);

    /* Door closed right after entering: relock at once, not after 5 s. */
    CHECK_EQ(lock_app_access(&app, SLP_CRED_CARD_UID, uid, 4), SLP_RESULT_OK);
    lock_app_door(&app, true);
    hal_host_advance(1500);
    lock_app_door(&app, false);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED);
    CHECK(!hal_host.clutch_engaged);

    /* Door left open: alarm after door_alarm_s, once. */
    drain(0, NULL);
    lock_app_door(&app, true);
    hal_host_advance(119000);
    lock_app_tick(&app);
    uint8_t alarm = 0;
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 0);
    hal_host_advance(2000);
    lock_app_tick(&app);
    lock_app_tick(&app);
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 1);
    CHECK_EQ(alarm, SLP_ALARM_DOOR_OPEN);

    lock_app_tamper(&app);
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 1);
    CHECK_EQ(alarm, SLP_ALARM_TAMPER);
    lock_app_mech_key(&app);
    CHECK_EQ(drain(SLP_EVT_ALARM, &alarm), 1);
    CHECK_EQ(alarm, SLP_ALARM_MECH_KEY);
}

static void test_offline_passwords(void)
{
    setup();
    uint32_t now = hal_unix_time();
    uint32_t h = now / 3600u;
    uint8_t code[OFP_LEN + 1] = { 0 };

    /* Known-answer value shared with the Go implementation
     * (common/slpgo/slp/local_test.go). */
    CHECK_EQ(ofp_compute(3, 1, 500000, code), 0);
    printf("    ofp(kind 3, index 1, hour 500000) = %s\n", code);
    CHECK(memcmp(code, "31", 2) == 0);

    /* One-time code issued 2 hours ago: works once. */
    ofp_compute(OFP_KIND_ONCE, 0, h - 2, code);
    CHECK_EQ(keypad((const char *)code), SLP_RESULT_OK);
    relock();
    CHECK_EQ(keypad((const char *)code), SLP_RESULT_EXPIRED);
    /* Issued 7 hours ago: too old. */
    ofp_compute(OFP_KIND_ONCE, 1, h - 7, code);
    CHECK_EQ(keypad((const char *)code), SLP_RESULT_DENIED);

    /* 8-hour code (kind 3) issued 5 hours ago: reusable within the window. */
    uint8_t timed[OFP_LEN + 1] = { 0 };
    ofp_compute(3, 2, h - 5, timed);
    CHECK_EQ(keypad((const char *)timed), SLP_RESULT_OK);
    relock();
    CHECK_EQ(keypad((const char *)timed), SLP_RESULT_OK);
    relock();
    /* Clear code revokes it. */
    ofp_compute(OFP_KIND_CLEAR, 0, h, code);
    CHECK_EQ(keypad((const char *)code), SLP_RESULT_OK);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED); /* clear code does not open */
    CHECK_EQ(keypad((const char *)timed), SLP_RESULT_EXPIRED);

    /* Expired after the window. */
    ofp_compute(1, 3, h, code); /* 1 hour class */
    hal_set_unix_time(now + 2 * 3600);
    CHECK_EQ(keypad((const char *)code), SLP_RESULT_DENIED);
    /* Records never carry the digits: only kind + index. */
    uint8_t buf[96];
    size_t n = lock_app_pop_event(&app, buf, sizeof(buf));
    slp_app_reader_t r;
    slp_app_hdr_t hd;
    const uint8_t *v;
    uint8_t l;
    slp_app_parse(&r, buf, n, &hd);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_CRED_ID, &v, &l), 1);
    CHECK_EQ(l, 2);
}

static void make_ble_token(uint32_t user, uint32_t valid_to, uint8_t flags, uint8_t tok[17])
{
    uint8_t msg[12] = { 'B', 'L', 'E' };
    for (int i = 0; i < 4; i++) {
        tok[i] = (uint8_t)(user >> (8 * i));
        tok[4 + i] = (uint8_t)(valid_to >> (8 * i));
    }
    tok[8] = flags;
    memcpy(msg + 3, tok, 9);
    hal_app_mac(msg, sizeof(msg), tok + 9);
}

static void test_ble_offline_token(void)
{
    setup();
    uint8_t tok[17];
    uint32_t now = hal_unix_time();
    make_ble_token(9001, now + 3600, 0, tok);
    CHECK_EQ(lock_app_ble(&app, tok, 17), SLP_RESULT_OK);
    relock();
    tok[0] ^= 1; /* forged user id */
    CHECK_EQ(lock_app_ble(&app, tok, 17), SLP_RESULT_DENIED);
    make_ble_token(9001, now - 1, 0, tok);
    CHECK_EQ(lock_app_ble(&app, tok, 17), SLP_RESULT_EXPIRED);

    /* Dorm keeper token works during lockdown, a student token does not. */
    CHECK_EQ(send_simple(SLP_CMD_LOCKDOWN, 600, 0), SLP_RESULT_OK);
    make_ble_token(9001, now + 3600, 0, tok);
    CHECK_EQ(lock_app_ble(&app, tok, 17), SLP_RESULT_DENIED);
    make_ble_token(1, now + 3600, SLP_CRED_F_ADMIN, tok);
    CHECK_EQ(lock_app_ble(&app, tok, 17), SLP_RESULT_OK);
}

static void test_remote_unlock_needs_request(void)
{
    setup();
    uint8_t req[64];
    slp_app_writer_t w;
    build(req, sizeof(req), SLP_CMD_CONFIG, 700, 0, &w);
    slp_app_put_u8(&w, SLP_TLV_CFG_REMOTE, 0);
    slp_app_put_u16(&w, SLP_TLV_CFG_AUTOLOCK_S, 8);
    CHECK_EQ(send_cmd(&w, req, NULL), SLP_RESULT_OK);
    CHECK_EQ(app.core.default_open_s, 8);
    CHECK_EQ(send_simple(SLP_CMD_UNLOCK, 701, 0), SLP_RESULT_NEED_REQUEST);

    uint16_t id = lock_app_doorbell(&app);
    CHECK_EQ(drain(SLP_EVT_UNLOCK_REQ, NULL), 1);
    build(req, sizeof(req), SLP_CMD_UNLOCK, 702, 0, &w);
    slp_app_put_u16(&w, SLP_TLV_REQ_ID, (uint16_t)(id + 1)); /* wrong request */
    CHECK_EQ(send_cmd(&w, req, NULL), SLP_RESULT_NEED_REQUEST);
    build(req, sizeof(req), SLP_CMD_UNLOCK, 703, 0, &w);
    slp_app_put_u16(&w, SLP_TLV_REQ_ID, id);
    CHECK_EQ(send_cmd(&w, req, NULL), SLP_RESULT_OK);
    hal_host_advance(7900);
    lock_app_tick(&app);
    CHECK_EQ(app.core.state, LOCK_ST_UNLOCKED); /* configured 8 s */
    hal_host_advance(200);
    lock_app_tick(&app);
    CHECK_EQ(app.core.state, LOCK_ST_LOCKED);

    /* The request expires after its window. */
    id = lock_app_doorbell(&app);
    hal_host_advance(LOCK_REQ_WINDOW_S * 1000u + 1);
    lock_app_tick(&app);
    build(req, sizeof(req), SLP_CMD_UNLOCK, 704, 0, &w);
    slp_app_put_u16(&w, SLP_TLV_REQ_ID, id);
    CHECK_EQ(send_cmd(&w, req, NULL), SLP_RESULT_NEED_REQUEST);
}

static void test_read_log_and_time_sync(void)
{
    setup();
    const uint8_t bad[4] = { 3, 3, 3, 3 };
    app.cfg.wrong_limit = 0;
    for (int i = 0; i < 30; i++) {
        lock_app_access(&app, SLP_CRED_CARD_UID, bad, 4);
    }
    drain(0, NULL); /* the radio sent 16, 14 were dropped while offline */
    uint8_t req[64];
    slp_app_writer_t w;
    uint16_t count = 0;
    build(req, sizeof(req), SLP_CMD_READ_LOG, 800, 0, &w);
    slp_app_put_u32(&w, SLP_TLV_LOG_SEQ, 1);
    slp_app_put_u16(&w, SLP_TLV_COUNT, 14);
    CHECK_EQ(send_cmd(&w, req, &count), SLP_RESULT_OK);
    CHECK_EQ(count, 14);
    uint8_t buf[96];
    size_t n = lock_app_pop_event(&app, buf, sizeof(buf));
    slp_app_reader_t r;
    slp_app_hdr_t h;
    const uint8_t *v;
    uint8_t l;
    slp_app_parse(&r, buf, n, &h);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_LOG_SEQ, &v, &l), 1);
    CHECK_EQ(slp_le32(v), 1u);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_REPLAYED, &v, &l), 1);

    /* Beacon time sync: small differences are ignored, drift is fixed. */
    uint32_t t = hal_unix_time();
    CHECK_EQ(lock_app_time_sync(&app, t + 1), 0);
    CHECK_EQ(lock_app_time_sync(&app, t + 90), 90);
    CHECK_EQ(hal_unix_time(), t + 90);

    /* Heartbeat interval: configured, x4 on critical battery, jittered. */
    app.cfg.heartbeat_s = 100;
    CHECK_EQ(lock_app_heartbeat_ms(&app, 10000), 100000u);
    CHECK_EQ(lock_app_heartbeat_ms(&app, 0), 90000u);
    app.batt_level = SLP_BATT_CRITICAL;
    CHECK_EQ(lock_app_heartbeat_ms(&app, 40000), 400000u);
}

static void test_uplink_queue_ack_and_retry(void)
{
    lock_upq_t q;
    lock_upq_init(&q);
    uint8_t m[10] = { 1, 2, 3 };
    CHECK(lock_upq_push(&q, m, 10));
    CHECK(lock_upq_push(&q, m, 5));
    const lock_upq_item_t *it = lock_upq_due(&q, 0);
    CHECK(it != NULL && it->len == 10);
    lock_upq_sent(&q, 7, 0);
    CHECK(lock_upq_due(&q, 100) == NULL); /* waiting for the ACK */
    CHECK(!lock_upq_on_ack(&q, 8));       /* someone else's ACK */
    CHECK(!lock_upq_check_timeout(&q, 299));
    CHECK(!lock_upq_check_timeout(&q, 300)); /* retry */
    it = lock_upq_due(&q, 300);
    CHECK(it != NULL && it->len == 10);
    lock_upq_sent(&q, 9, 300); /* next wait 600 ms */
    CHECK(!lock_upq_check_timeout(&q, 899));
    CHECK(lock_upq_on_ack(&q, 9));
    it = lock_upq_due(&q, 900);
    CHECK(it != NULL && it->len == 5);
    /* Gives up after LOCK_UPQ_TRIES. */
    uint32_t t = 1000;
    for (unsigned i = 0; i < LOCK_UPQ_TRIES; i++) {
        CHECK(lock_upq_due(&q, t) != NULL);
        lock_upq_sent(&q, (uint8_t)i, t);
        t += 100000;
        bool dropped = lock_upq_check_timeout(&q, t);
        CHECK_EQ(dropped, i == LOCK_UPQ_TRIES - 1);
    }
    CHECK_EQ(q.count, 0);
    CHECK_EQ(q.dropped, 1u);
}

static void test_mac_loss_and_rejoin_backoff(void)
{
    lock_mac_t m;
    lock_mac_init(&m, 0x0101, 0x0042, 0, 3, 1000, 20);
    CHECK(!lock_mac_on_beacon(&m, 1000, 0));
    CHECK(!lock_mac_on_beacon(&m, 2000, 0));
    CHECK(lock_mac_on_beacon(&m, 2000 + LOCK_MAC_LOST_MS + 1000, 0)); /* came back */
    CHECK_EQ(m.lost_count, 1u);
    CHECK(!lock_mac_need_rejoin(&m, 10000));
    CHECK(lock_mac_need_rejoin(&m, 8000 + LOCK_MAC_REJOIN_MS + 1));
    CHECK_EQ(lock_mac_join_backoff(&m, 0), 3000u);
    CHECK_EQ(lock_mac_join_backoff(&m, 0), 6000u);
    CHECK_EQ(lock_mac_join_backoff(&m, 0), 12000u);
    for (int i = 0; i < 10; i++) lock_mac_join_backoff(&m, 0);
    CHECK_EQ(lock_mac_join_backoff(&m, 0), LOCK_MAC_JOIN_MAX_MS);
    CHECK(lock_mac_join_backoff(&m, 0xFFFFFFFFu) <= LOCK_MAC_JOIN_MAX_MS * 3 / 2);
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
    RUN(test_access_events_queued);
    RUN(test_battery_levels_and_hysteresis);
    RUN(test_event_queue_overflow_keeps_newest);
    RUN(test_passwords_decoy_one_time_schedule);
    RUN(test_wrong_attempts_lockout);
    RUN(test_duress_freeze_and_move_out);
    RUN(test_double_lock_door_sensor_and_alarms);
    RUN(test_offline_passwords);
    RUN(test_ble_offline_token);
    RUN(test_remote_unlock_needs_request);
    RUN(test_read_log_and_time_sync);
    RUN(test_uplink_queue_ack_and_retry);
    RUN(test_mac_loss_and_rejoin_backoff);
    UNIT_DONE();
}
