/*
 * Two-layer protection on top of the crypto port (slp/crypto_port.h).
 *
 * End-to-end (platform <-> lock, key K_app, MIC 8). The gateway relays these
 * bytes without being able to read or forge them.
 *
 *   downlink:  HDR(9) | ENC(TLV...) | MIC(8)
 *              nonce = lock EUI | MSG_ID | 0x06          AAD = HDR
 *   uplink:    UPSEQ(4) | HDR(9) | ENC(TLV...) | MIC(8)
 *              nonce = lock EUI | UPSEQ  | 0x46          AAD = UPSEQ | HDR
 *
 * UPSEQ is the lock's strictly increasing uplink counter. It exists because
 * an uplink result echoes the request MSG_ID, and a retransmitted request
 * produces a second result for the same MSG_ID: using MSG_ID as the nonce
 * would then reuse a nonce. The platform also rejects UPSEQ replays.
 * Group (broadcast) downlinks use K_grp_app and the gateway EUI in the nonce.
 *
 * Link layer (lock <-> gateway, key K_link or K_grp, MIC 4): see
 * slp/frame.h. The whole MAC header is the AAD, the payload is encrypted,
 * nonce = sender EUI | frame counter | 0x05.
 */
#ifndef SLP_SECURE_H
#define SLP_SECURE_H

#include "slp/app.h"
#include "slp/crypto_port.h"
#include "slp/frame.h"
#include "slp/security.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_DIR_DOWN 0x00u
#define SLP_DIR_UP   0x40u
#define SLP_UPSEQ_LEN 4u

/* Seals a plaintext app message in place. buf must have room for len + 8
 * (downlink) or len + 12 (uplink: UPSEQ is prepended). */
int slp_app_seal_down(slp_key_handle_t key, uint64_t nonce_eui, uint8_t *buf, size_t len,
                      size_t cap, size_t *out_len);
int slp_app_seal_up(slp_key_handle_t key, uint64_t lock_eui, uint32_t upseq, uint8_t *buf,
                    size_t len, size_t cap, size_t *out_len);

/* Verifies and decrypts in place. On success the plaintext app message
 * (HDR | TLV...) starts at buf (downlink) or buf + 4 (uplink). */
int slp_app_open_down(slp_key_handle_t key, uint64_t nonce_eui, uint8_t *buf, size_t len,
                      size_t *plain_len);
int slp_app_open_up(slp_key_handle_t key, uint64_t lock_eui, uint8_t *buf, size_t len,
                    uint32_t *upseq, size_t *plain_len);

/* Builds a secured frame: f->payload is encrypted with `key` (MIC 4).
 * f->security must be true and f->frame_counter set. */
int slp_frame_encode_secure(slp_key_handle_t key, uint64_t sender_eui, const slp_frame_t *f,
                            uint8_t *buf, size_t cap, size_t *out_len);

/* Decrypts a decoded secured frame in place (buf is the frame passed to
 * slp_frame_decode). On success f->payload/payload_len describe plaintext.
 * Does NOT check the frame counter: call the anti-replay check after this. */
int slp_frame_open_secure(slp_key_handle_t key, uint64_t sender_eui, uint8_t *buf,
                          slp_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* SLP_SECURE_H */
