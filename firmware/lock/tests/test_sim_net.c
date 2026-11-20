/* Simulator conventions and C <-> Go interoperability vectors
 * (same values as common/slpgo/simnet/simnet_test.go and slp/vector_test.go). */
#include <string.h>

#include "sim_net.h"
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

static void test_sim_key_matches_go(void)
{
    uint8_t k[16], want[16];
    sim_derive_key(SIM_TAG_APP, SIM_LOCK_EUI(1, 1, 1), k);
    hex("20e3778dea1708a897213045be9b2f3e", want);
    CHECK(memcmp(k, want, 16) == 0);
}

static void test_app_seal_matches_go(void)
{
    slp_host_key_set(1, (const uint8_t *)"0123456789abcdef");
    uint8_t buf[64], want[64];
    slp_app_writer_t w;
    slp_app_hdr_t h = { .cmd = SLP_CMD_UNLOCK, .msg_id = 0x01020304, .expires_at = 0x6A000000 };
    slp_app_begin(&w, buf, sizeof(buf), &h);
    slp_app_put_u16(&w, SLP_TLV_DURATION_S, 5);
    size_t n;
    CHECK_EQ(slp_app_seal_down(1, 0x5A00000000010101ull, buf, w.len, sizeof(buf), &n), SLP_OK);
    int wn = hex("01040302010000006af94db7bbdb4faa19b8e75475", want);
    CHECK_EQ((int)n, wn);
    CHECK(memcmp(buf, want, n) == 0);
}

int main(void)
{
    printf("sim/interop tests\n");
    RUN(test_sim_key_matches_go);
    RUN(test_app_seal_matches_go);
    UNIT_DONE();
}
