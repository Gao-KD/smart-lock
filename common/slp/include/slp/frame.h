/*
 * MAC frame encode / decode.
 *
 * On-air layout (after PHY preamble, sync word and PHR length byte):
 *
 *   FCF(2) SEQ(1) PAN(2) DST(0/2/8) SRC(0/2/8) [FC(4) KEYID(1)] PAYLOAD(n) CRC16(2)
 *
 * All multi-byte fields are little-endian. When security is enabled the
 * payload is the AES-CCM* ciphertext followed by the 4-byte link MIC, and the
 * header (everything before the payload) is the CCM additional data.
 *
 * FCF bits:
 *   [2:0]   frame type (slp_frame_type_t)
 *   [3]     security enabled
 *   [4]     frame pending (more frames follow, receiver keeps RX on)
 *   [5]     ACK request
 *   [9:8]   destination address mode (slp_addr_mode_t)
 *   [11:10] source address mode
 *   [13:12] protocol version
 */
#ifndef SLP_FRAME_H
#define SLP_FRAME_H

#include "slp/slp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLP_FT_BEACON  = 0,
    SLP_FT_WAKEUP  = 1, /* short wake-up frame of an asynchronous wake-up train */
    SLP_FT_DATA    = 2,
    SLP_FT_ACK     = 3,
    SLP_FT_MAC_CMD = 4  /* join, slot assignment, ... */
} slp_frame_type_t;

typedef enum {
    SLP_ADDR_NONE  = 0,
    SLP_ADDR_SHORT = 2,
    SLP_ADDR_LONG  = 3
} slp_addr_mode_t;

typedef struct {
    uint8_t  type;          /* slp_frame_type_t */
    bool     security;
    bool     frame_pending;
    bool     ack_request;
    uint8_t  version;

    uint8_t  seq;
    uint16_t pan_id;

    uint8_t  dst_mode;      /* slp_addr_mode_t */
    uint64_t dst;
    uint8_t  src_mode;
    uint64_t src;

    /* Valid only when security == true. */
    uint32_t frame_counter;
    uint8_t  key_id;

    const uint8_t *payload;
    size_t   payload_len;
} slp_frame_t;

/* Length of the MAC header (CCM additional data) for this frame. */
size_t slp_frame_header_len(const slp_frame_t *f);

/* Encode f into buf. Appends CRC16. *out_len receives the total length. */
int slp_frame_encode(const slp_frame_t *f, uint8_t *buf, size_t cap, size_t *out_len);

/* Decode and CRC-check buf. out->payload points into buf (no copy). */
int slp_frame_decode(const uint8_t *buf, size_t len, slp_frame_t *out);

/* CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF). */
uint16_t slp_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* SLP_FRAME_H */
