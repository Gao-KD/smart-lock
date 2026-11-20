/* AES / CCM* host backend and the two-layer seal/open helpers. */
#include <string.h>

#include "slp/crypto_host.h"
#include "slp/secure.h"
#include "unit.h"

static int hex(const char *s, uint8_t *out)
{
    int n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void test_aes_fips197(void)
{
    uint8_t k[16], p[16], c[16], expect[16];
    hex("000102030405060708090a0b0c0d0e0f", k);
    hex("00112233445566778899aabbccddeeff", p);
    hex("69c4e0d86a7b0430d8cdb78070b4c55a", expect);
    slp_aes128_encrypt(k, p, c);
    CHECK(memcmp(c, expect, 16) == 0);
}

static void test_ccm_rfc3610_vector1(void)
{
    uint8_t k[16], nonce[13], aad[8], buf[64], expect[64];
    for (int i = 0; i < 16; i++) k[i] = (uint8_t)(0xC0 + i);
    hex("00000003020100a0a1a2a3a4a5", nonce);
    for (int i = 0; i < 8; i++) aad[i] = (uint8_t)i;
    for (int i = 0; i < 23; i++) buf[i] = (uint8_t)(8 + i);
    int n = hex("588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0", expect);
    CHECK_EQ(slp_ccm_encrypt_raw(k, nonce, aad, 8, buf, 23, 8), SLP_OK);
    CHECK_EQ(n, 31);
    CHECK(memcmp(buf, expect, 31) == 0);
    CHECK_EQ(slp_ccm_decrypt_raw(k, nonce, aad, 8, buf, 31, 8), SLP_OK);
    CHECK_EQ(buf[0], 8);
    CHECK_EQ(buf[22], 30);
    /* tamper */
    CHECK_EQ(slp_ccm_encrypt_raw(k, nonce, aad, 8, buf, 23, 4), SLP_OK);
    buf[3] ^= 1;
    CHECK_EQ(slp_ccm_decrypt_raw(k, nonce, aad, 8, buf, 27, 4), SLP_ERR_FORMAT);
}

static void test_app_seal_roundtrip(void)
{
    uint8_t key[16] = { 1, 2, 3 };
    slp_host_key_set(7, key);
    uint8_t buf[64];
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_CMD_UNLOCK, .msg_id = 42, .expires_at = 0 };
    slp_app_begin(&w, buf, sizeof(buf), &h);
    slp_app_put_u16(&w, SLP_TLV_DURATION_S, 9);
    size_t plain = w.len, sealed, opened;
    CHECK_EQ(slp_app_seal_down(7, 0x1122u, buf, plain, sizeof(buf), &sealed), SLP_OK);
    CHECK_EQ(sealed, plain + 8);
    CHECK_EQ(buf[0], SLP_CMD_UNLOCK); /* header stays readable */
    CHECK_EQ(slp_app_open_down(7, 0x1122u, buf, sealed, &opened), SLP_OK);
    CHECK_EQ(opened, plain);
    slp_app_reader_t r;
    slp_app_hdr_t rh;
    const uint8_t *v;
    uint8_t l;
    CHECK_EQ(slp_app_parse(&r, buf, opened, &rh), SLP_OK);
    CHECK_EQ(slp_app_find(&r, SLP_TLV_DURATION_S, &v, &l), 1);
    CHECK_EQ(slp_le16(v), 9);

    /* Wrong lock (nonce) or a changed header fails. */
    slp_app_seal_down(7, 0x1122u, buf, plain, sizeof(buf), &sealed);
    CHECK_EQ(slp_app_open_down(7, 0x9999u, buf, sealed, &opened), SLP_ERR_FORMAT);
    slp_app_begin(&w, buf, sizeof(buf), &h);
    slp_app_seal_down(7, 0x1122u, buf, w.len, sizeof(buf), &sealed);
    buf[1] ^= 1; /* msg id */
    CHECK_EQ(slp_app_open_down(7, 0x1122u, buf, sealed, &opened), SLP_ERR_FORMAT);

    /* Uplink. */
    h.cmd = SLP_RSP_RESULT;
    slp_app_begin(&w, buf, sizeof(buf), &h);
    slp_app_put_u8(&w, SLP_TLV_RESULT, 0);
    plain = w.len;
    CHECK_EQ(slp_app_seal_up(7, 0x1122u, 5, buf, plain, sizeof(buf), &sealed), SLP_OK);
    CHECK_EQ(sealed, plain + 12);
    uint32_t seq = 0;
    CHECK_EQ(slp_app_open_up(7, 0x1122u, buf, sealed, &seq, &opened), SLP_OK);
    CHECK_EQ(seq, 5u);
    CHECK_EQ(opened, plain);
    CHECK_EQ(buf[4], SLP_RSP_RESULT);
}

static void test_secure_frame(void)
{
    uint8_t key[16] = { 9 };
    slp_host_key_set(1, key);
    const uint8_t pl[] = { 1, 2, 3, 4, 5, 6, 7 };
    slp_frame_t f = {
        .type = SLP_FT_DATA, .security = true, .ack_request = true, .seq = 3, .pan_id = 0x5A01,
        .dst_mode = SLP_ADDR_SHORT, .dst = 0x0021, .src_mode = SLP_ADDR_SHORT, .src = 0,
        .frame_counter = 100, .key_id = SLP_KEY_LINK, .payload = pl, .payload_len = sizeof(pl),
    };
    uint8_t buf[128];
    size_t len;
    CHECK_EQ(slp_frame_encode_secure(1, 0xAAu, &f, buf, sizeof(buf), &len), SLP_OK);
    slp_frame_t d;
    CHECK_EQ(slp_frame_decode(buf, len, &d), SLP_OK);
    CHECK(memcmp(d.payload, pl, sizeof(pl)) != 0); /* encrypted on air */
    CHECK_EQ(slp_frame_open_secure(1, 0xAAu, buf, &d), SLP_OK);
    CHECK_EQ(d.payload_len, sizeof(pl));
    CHECK(memcmp(d.payload, pl, sizeof(pl)) == 0);

    /* Forged frame counter (attacker edits header, fixes CRC) fails MIC. */
    slp_frame_encode_secure(1, 0xAAu, &f, buf, sizeof(buf), &len);
    slp_frame_decode(buf, len, &d);
    size_t fc_off = (size_t)(d.payload - buf) - 5;
    buf[fc_off] ^= 0x10;
    uint16_t crc = slp_crc16(buf, len - 2);
    buf[len - 2] = (uint8_t)crc;
    buf[len - 1] = (uint8_t)(crc >> 8);
    CHECK_EQ(slp_frame_decode(buf, len, &d), SLP_OK);
    CHECK_EQ(slp_frame_open_secure(1, 0xAAu, buf, &d), SLP_ERR_FORMAT);
}

int main(void)
{
    printf("secure tests\n");
    RUN(test_aes_fips197);
    RUN(test_ccm_rfc3610_vector1);
    RUN(test_app_seal_roundtrip);
    RUN(test_secure_frame);
    UNIT_DONE();
}
