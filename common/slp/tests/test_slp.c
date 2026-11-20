#include <string.h>

#include "slp/app.h"
#include "slp/frame.h"
#include "slp/security.h"
#include "slp/slot.h"
#include "unit.h"

static void test_crc16_check_value(void)
{
    const uint8_t s[] = "123456789";
    CHECK_EQ(slp_crc16(s, 9), 0x29B1);
}

static void test_frame_roundtrip_secured_data(void)
{
    const uint8_t payload[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    slp_frame_t f = {
        .type = SLP_FT_DATA, .security = true, .ack_request = true,
        .seq = 42, .pan_id = 0x1234,
        .dst_mode = SLP_ADDR_SHORT, .dst = 0x0007,
        .src_mode = SLP_ADDR_SHORT, .src = 0x0001,
        .frame_counter = 0xA1B2C3D4, .key_id = 1,
        .payload = payload, .payload_len = sizeof(payload),
    };
    uint8_t buf[SLP_MAX_FRAME_LEN];
    size_t len = 0;
    CHECK_EQ(slp_frame_encode(&f, buf, sizeof(buf), &len), SLP_OK);
    CHECK_EQ(slp_frame_header_len(&f), 2u + 1 + 2 + 2 + 2 + 5);
    CHECK_EQ(len, slp_frame_header_len(&f) + sizeof(payload) + 2);

    slp_frame_t d;
    CHECK_EQ(slp_frame_decode(buf, len, &d), SLP_OK);
    CHECK_EQ(d.type, SLP_FT_DATA);
    CHECK(d.security && d.ack_request && !d.frame_pending);
    CHECK_EQ(d.seq, 42);
    CHECK_EQ(d.pan_id, 0x1234);
    CHECK_EQ(d.dst, 0x0007u);
    CHECK_EQ(d.src, 0x0001u);
    CHECK_EQ(d.frame_counter, 0xA1B2C3D4u);
    CHECK_EQ(d.key_id, 1);
    CHECK_EQ(d.payload_len, sizeof(payload));
    CHECK(memcmp(d.payload, payload, sizeof(payload)) == 0);
}

static void test_frame_long_address_join(void)
{
    slp_frame_t f = {
        .type = SLP_FT_MAC_CMD, .pan_id = 0xFFFF,
        .dst_mode = SLP_ADDR_SHORT, .dst = SLP_SHORT_BROADCAST,
        .src_mode = SLP_ADDR_LONG, .src = 0x0011223344556677ull,
    };
    uint8_t buf[64];
    size_t len;
    CHECK_EQ(slp_frame_encode(&f, buf, sizeof(buf), &len), SLP_OK);
    slp_frame_t d;
    CHECK_EQ(slp_frame_decode(buf, len, &d), SLP_OK);
    CHECK_EQ(d.src_mode, SLP_ADDR_LONG);
    CHECK_EQ(d.src, 0x0011223344556677ull);
    CHECK_EQ(d.payload_len, 0u);
}

static void test_frame_rejects_corruption(void)
{
    const uint8_t payload[] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE };
    slp_frame_t f = {
        .type = SLP_FT_DATA, .dst_mode = SLP_ADDR_SHORT, .dst = 9,
        .src_mode = SLP_ADDR_SHORT, .src = 1,
        .payload = payload, .payload_len = sizeof(payload),
    };
    uint8_t buf[64];
    size_t len;
    CHECK_EQ(slp_frame_encode(&f, buf, sizeof(buf), &len), SLP_OK);
    buf[8] ^= 0x01;
    slp_frame_t d;
    CHECK_EQ(slp_frame_decode(buf, len, &d), SLP_ERR_CRC);
    CHECK_EQ(slp_frame_decode(buf, 3, &d), SLP_ERR_FORMAT);
}

static void test_frame_size_limits(void)
{
    uint8_t big[200] = { 0 };
    slp_frame_t f = {
        .type = SLP_FT_DATA, .dst_mode = SLP_ADDR_SHORT, .src_mode = SLP_ADDR_SHORT,
        .payload = big, .payload_len = 150,
    };
    uint8_t buf[256];
    size_t len;
    CHECK_EQ(slp_frame_encode(&f, buf, sizeof(buf), &len), SLP_ERR_ARG);
    f.payload_len = 20;
    CHECK_EQ(slp_frame_encode(&f, buf, 10, &len), SLP_ERR_NOSPACE);
}

static void test_nonce_layout(void)
{
    uint8_t n[SLP_NONCE_LEN];
    slp_ccm_nonce(0x0102030405060708ull, 0x0A0B0C0Du, SLP_SEC_ENC_MIC32, n);
    const uint8_t expect[SLP_NONCE_LEN] = { 1, 2, 3, 4, 5, 6, 7, 8, 0x0A, 0x0B, 0x0C, 0x0D, 5 };
    CHECK(memcmp(n, expect, sizeof(expect)) == 0);
}

static void test_replay(void)
{
    slp_replay_t r;
    slp_replay_reset(&r);
    CHECK_EQ(slp_replay_check_and_update(&r, 0), SLP_OK);
    CHECK_EQ(slp_replay_check_and_update(&r, 0), SLP_ERR_REPLAY);
    CHECK_EQ(slp_replay_check_and_update(&r, 5), SLP_OK);
    CHECK_EQ(slp_replay_check_and_update(&r, 4), SLP_ERR_REPLAY);
    CHECK_EQ(slp_replay_check_and_update(&r, 6), SLP_OK);
}

static void test_txctr_never_reuses_after_reboot(void)
{
    slp_txctr_t c;
    slp_txctr_restore(&c, 0);
    uint32_t fc, persisted = 0, pv;
    bool np;
    for (int i = 0; i < 300; i++) {
        CHECK_EQ(slp_txctr_take(&c, &fc, &np, &pv), SLP_OK);
        CHECK_EQ(fc, (uint32_t)i);
        if (np) {
            persisted = pv;
        }
    }
    CHECK_EQ(persisted, 2u * SLP_TXCTR_STEP);
    /* Simulated reboot: restart from the persisted bound. */
    slp_txctr_restore(&c, persisted);
    CHECK_EQ(slp_txctr_take(&c, &fc, &np, &pv), SLP_OK);
    CHECK(fc >= 300u);
    CHECK(np);

    slp_txctr_restore(&c, UINT32_MAX);
    CHECK_EQ(slp_txctr_take(&c, &fc, &np, &pv), SLP_ERR_EXHAUSTED);
}

static void test_msgid_cache(void)
{
    slp_msgid_cache_t c;
    slp_msgid_cache_reset(&c);
    CHECK(!slp_msgid_seen_or_add(&c, 100));
    CHECK(slp_msgid_seen_or_add(&c, 100));
    for (uint32_t i = 0; i < SLP_MSGID_CACHE_SIZE; i++) {
        CHECK(!slp_msgid_seen_or_add(&c, 1000 + i));
    }
    /* 100 has been evicted by 16 newer ids. */
    CHECK(!slp_msgid_seen_or_add(&c, 100));
}

static void test_app_message_roundtrip(void)
{
    uint8_t buf[64];
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_CMD_CRED_ADD, .msg_id = 0xDEADBEEF, .expires_at = 1800000000u };
    const uint8_t uid[4] = { 0x12, 0x34, 0x56, 0x78 };
    CHECK_EQ(slp_app_begin(&w, buf, sizeof(buf), &h), SLP_OK);
    CHECK_EQ(slp_app_put_u8(&w, SLP_TLV_CRED_TYPE, SLP_CRED_CARD_UID), SLP_OK);
    CHECK_EQ(slp_app_put(&w, SLP_TLV_CRED_ID, uid, sizeof(uid)), SLP_OK);
    CHECK_EQ(slp_app_put_u32(&w, SLP_TLV_VALID_TO, 1900000000u), SLP_OK);

    slp_app_reader_t r;
    slp_app_hdr_t d;
    CHECK_EQ(slp_app_parse(&r, buf, w.len, &d), SLP_OK);
    CHECK_EQ(d.cmd, SLP_CMD_CRED_ADD);
    CHECK_EQ(d.msg_id, 0xDEADBEEFu);
    CHECK_EQ(d.expires_at, 1800000000u);

    const uint8_t *v;
    uint8_t l;
    CHECK_EQ(slp_app_find(&r, SLP_TLV_CRED_ID, &v, &l), 1);
    CHECK_EQ(l, 4);
    CHECK(memcmp(v, uid, 4) == 0);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_VALID_TO, &v, &l), 1);
    CHECK_EQ(slp_le32(v), 1900000000u);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_DURATION_S, &v, &l), 0);

    /* Truncated TLV is an error, not a crash. */
    CHECK_EQ(slp_app_parse(&r, buf, w.len - 1, &d), SLP_OK);
    CHECK(slp_app_find(&r, SLP_TLV_DURATION_S, &v, &l) < 0);
}

static void test_slot_timing(void)
{
    slp_slot_cfg_t c = { .epoch_ms = 1000, .period_ms = 1000, .slot_ms = 20 };
    CHECK_EQ(slp_slots_per_period(&c), 50);
    CHECK_EQ(slp_slot_current(&c, 1000), 0);
    CHECK_EQ(slp_slot_current(&c, 1459), 22);
    CHECK_EQ(slp_slot_next_start(&c, 1000, 0), 1000u);
    CHECK_EQ(slp_slot_next_start(&c, 1001, 0), 2000u);
    CHECK_EQ(slp_slot_next_start(&c, 1459, 25), 1500u);
    CHECK_EQ(slp_slot_next_start(&c, 1501, 25), 2500u);

    /* Wrap-around of the 32-bit millisecond clock. */
    slp_slot_cfg_t w = { .epoch_ms = 0xFFFFFF00u, .period_ms = 1000, .slot_ms = 20 };
    uint32_t now = 0x00000010u; /* 0x110 = 272 ms after epoch */
    CHECK_EQ(slp_slot_current(&w, now), 13);
    CHECK_EQ(slp_slot_next_start(&w, now, 14), 0xFFFFFF00u + 280u);

    slp_slot_resync(&c, 5555, 10);
    CHECK_EQ(slp_slot_current(&c, 5555), 10);
}

int main(void)
{
    printf("slp tests\n");
    RUN(test_crc16_check_value);
    RUN(test_frame_roundtrip_secured_data);
    RUN(test_frame_long_address_join);
    RUN(test_frame_rejects_corruption);
    RUN(test_frame_size_limits);
    RUN(test_nonce_layout);
    RUN(test_replay);
    RUN(test_txctr_never_reuses_after_reboot);
    RUN(test_msgid_cache);
    RUN(test_app_message_roundtrip);
    RUN(test_slot_timing);
    UNIT_DONE();
}
