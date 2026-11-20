#include "slp/security.h"

#include <string.h>

void slp_ccm_nonce(uint64_t src_eui, uint32_t frame_counter, uint8_t sec_level,
                   uint8_t out[SLP_NONCE_LEN])
{
    for (int i = 0; i < 8; i++) {
        out[i] = (uint8_t)(src_eui >> (8 * (7 - i)));
    }
    for (int i = 0; i < 4; i++) {
        out[8 + i] = (uint8_t)(frame_counter >> (8 * (3 - i)));
    }
    out[12] = sec_level;
}

void slp_replay_reset(slp_replay_t *r)
{
    r->last = 0;
    r->valid = false;
}

int slp_replay_check_and_update(slp_replay_t *r, uint32_t frame_counter)
{
    if (r == NULL) {
        return SLP_ERR_ARG;
    }
    if (r->valid && frame_counter <= r->last) {
        return SLP_ERR_REPLAY;
    }
    r->last = frame_counter;
    r->valid = true;
    return SLP_OK;
}

void slp_txctr_restore(slp_txctr_t *c, uint32_t stored)
{
    c->next = stored;
    c->reserved = stored;
}

int slp_txctr_take(slp_txctr_t *c, uint32_t *fc, bool *need_persist, uint32_t *persist_value)
{
    if (c == NULL || fc == NULL || need_persist == NULL || persist_value == NULL) {
        return SLP_ERR_ARG;
    }
    *need_persist = false;
    if (c->next == UINT32_MAX) {
        return SLP_ERR_EXHAUSTED;
    }
    if (c->next >= c->reserved) {
        uint32_t limit = (c->next > UINT32_MAX - SLP_TXCTR_STEP) ? UINT32_MAX
                                                                 : c->next + SLP_TXCTR_STEP;
        c->reserved = limit;
        *need_persist = true;
        *persist_value = limit;
    }
    *fc = c->next++;
    return SLP_OK;
}

void slp_msgid_cache_reset(slp_msgid_cache_t *c)
{
    memset(c, 0, sizeof(*c));
}

bool slp_msgid_seen_or_add(slp_msgid_cache_t *c, uint32_t id)
{
    for (uint8_t i = 0; i < c->count; i++) {
        if (c->ids[i] == id) {
            return true;
        }
    }
    c->ids[c->head] = id;
    c->head = (uint8_t)((c->head + 1u) % SLP_MSGID_CACHE_SIZE);
    if (c->count < SLP_MSGID_CACHE_SIZE) {
        c->count++;
    }
    return false;
}
