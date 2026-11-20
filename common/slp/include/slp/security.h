/*
 * Security helpers that do not depend on a crypto backend:
 *   - AES-CCM* nonce construction
 *   - receive-side anti-replay (strictly increasing frame counter)
 *   - transmit counter with batched persistence
 *   - end-to-end message id de-duplication cache
 *
 * The AES-CCM* primitive itself comes from the platform (PSA Crypto /
 * Secure Vault on EFR32), see slp/crypto_port.h.
 */
#ifndef SLP_SECURITY_H
#define SLP_SECURITY_H

#include "slp/slp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Security levels (CCM* M field). */
#define SLP_SEC_ENC_MIC32 0x05u
#define SLP_SEC_ENC_MIC64 0x06u

/* nonce = src EUI-64 (big-endian) | frame counter (big-endian) | level */
void slp_ccm_nonce(uint64_t src_eui, uint32_t frame_counter, uint8_t sec_level,
                   uint8_t out[SLP_NONCE_LEN]);

/* ---- receive-side anti-replay ---- */
typedef struct {
    uint32_t last;
    bool     valid;
} slp_replay_t;

void slp_replay_reset(slp_replay_t *r);
/* Accepts only counters strictly greater than the last accepted one.
 * Call this only after the MIC has been verified. */
int slp_replay_check_and_update(slp_replay_t *r, uint32_t frame_counter);

/* ---- transmit counter ----
 * Persisting the counter on every frame would wear the flash. Instead a
 * block of SLP_TXCTR_STEP counters is reserved by persisting the block's
 * upper bound; after a reboot counting resumes from that bound, so no
 * counter value is ever reused. */
#define SLP_TXCTR_STEP 256u

typedef struct {
    uint32_t next;
    uint32_t reserved; /* counters < reserved may be used without persisting */
} slp_txctr_t;

/* stored = value previously handed out through *persist_value (0 if none). */
void slp_txctr_restore(slp_txctr_t *c, uint32_t stored);

/* Returns SLP_OK and the counter in *fc. If *need_persist is set, the caller
 * MUST durably store *persist_value before transmitting the frame.
 * Returns SLP_ERR_EXHAUSTED when the key must be rotated. */
int slp_txctr_take(slp_txctr_t *c, uint32_t *fc, bool *need_persist, uint32_t *persist_value);

/* ---- message id de-duplication (end-to-end, cloud message ids) ---- */
#define SLP_MSGID_CACHE_SIZE 16u

typedef struct {
    uint32_t ids[SLP_MSGID_CACHE_SIZE];
    uint8_t  count;
    uint8_t  head;
} slp_msgid_cache_t;

void slp_msgid_cache_reset(slp_msgid_cache_t *c);
/* Returns true if id was already seen; otherwise records it and returns false. */
bool slp_msgid_seen_or_add(slp_msgid_cache_t *c, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* SLP_SECURITY_H */
