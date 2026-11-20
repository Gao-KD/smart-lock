#include "sim_net.h"

#include <string.h>

#include "slp/crypto_host.h"

void sim_derive_key(uint8_t tag, uint64_t eui, uint8_t out[16])
{
    static const uint8_t master[16] = { 's', 'm', 'a', 'r', 't', 'l', 'o', 'c',
                                        'k', '-', 's', 'i', 'm', '-', '0', '1' };
    uint8_t in[16] = { 0 };
    in[0] = tag;
    for (int i = 0; i < 8; i++) {
        in[8 + i] = (uint8_t)(eui >> (8 * (7 - i)));
    }
    slp_aes128_encrypt(master, in, out);
}
