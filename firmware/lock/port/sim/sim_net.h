/*
 * Simulator conventions shared with the Go side
 * (common/slpgo/simnet/simnet.go): layout, addressing, simulation-only key
 * derivation, and the UDP wire format gateway host <-> radio co-processor.
 */
#ifndef LOCK_SIM_NET_H
#define LOCK_SIM_NET_H

#include <stdint.h>

#define SIM_LOCK_EUI(b, f, r) (0x5A00000000000000ull | ((uint64_t)(b) << 16) | ((uint64_t)(f) << 8) | (uint64_t)(r))
#define SIM_GW_EUI(b, g)      (0x6A00000000000000ull | ((uint64_t)(b) << 8) | (uint64_t)(g))
#define SIM_PAN(b, g)         ((uint16_t)(0x5A00 + (b) * 16 + (g)))

#define SIM_TAG_LINK     'L'
#define SIM_TAG_APP      'A'
#define SIM_TAG_GRP_LINK 'G'
#define SIM_TAG_GRP_APP  'g'

/* Key = AES-128_master(tag | 0^7 | EUI big-endian), master = "smartlock-sim-01". */
void sim_derive_key(uint8_t tag, uint64_t eui, uint8_t out[16]);

enum {
    SIM_MSG_HELLO     = 0x01,
    SIM_MSG_PERIOD    = 0x02,
    SIM_MSG_TXNOW     = 0x03,
    SIM_MSG_ADD       = 0x04,
    SIM_MSG_TX_RESULT = 0x81,
    SIM_MSG_RX        = 0x82
};

enum { SIM_MAC_JOIN_REQ = 0x01, SIM_MAC_JOIN_ACCEPT = 0x02, SIM_MAC_PROBE = 0x03, SIM_MAC_SNIFF_DIV = 0x04 };

#endif
