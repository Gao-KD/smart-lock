#include "lock_mac.h"

#include <string.h>

void lock_mac_init(lock_mac_t *m, uint16_t pan_id, uint16_t short_addr, uint8_t channel,
                   uint16_t slot, uint16_t period_ms, uint16_t slot_ms)
{
    memset(m, 0, sizeof(*m));
    m->pan_id = pan_id;
    m->short_addr = short_addr;
    m->channel = channel;
    m->my_slot = slot;
    m->slot_cfg.period_ms = period_ms ? period_ms : SLP_DEFAULT_PERIOD_MS;
    m->slot_cfg.slot_ms = slot_ms ? slot_ms : SLP_DEFAULT_SLOT_MS;
    m->sync = LOCK_MAC_UNSYNCED;
    slp_replay_reset(&m->gw_replay);
    slp_replay_reset(&m->grp_replay);
}

bool lock_mac_on_beacon(lock_mac_t *m, uint32_t rx_ms, uint16_t beacon_slot)
{
    bool back = m->last_sync_ms != 0 && rx_ms - m->last_sync_ms > LOCK_MAC_LOST_MS;
    slp_slot_resync(&m->slot_cfg, rx_ms, beacon_slot);
    m->sync = LOCK_MAC_SYNCED;
    m->last_sync_ms = rx_ms ? rx_ms : 1;
    m->join_attempts = 0;
    if (back) {
        m->lost_count++;
    }
    return back;
}

bool lock_mac_need_rejoin(const lock_mac_t *m, uint32_t now)
{
    return m->last_sync_ms != 0 && now - m->last_sync_ms > LOCK_MAC_REJOIN_MS;
}

uint32_t lock_mac_join_backoff(lock_mac_t *m, uint32_t rnd)
{
    uint32_t d = LOCK_MAC_JOIN_MIN_MS;
    for (uint8_t i = 0; i < m->join_attempts && d < LOCK_MAC_JOIN_MAX_MS; i++) {
        d *= 2u;
    }
    if (d > LOCK_MAC_JOIN_MAX_MS) {
        d = LOCK_MAC_JOIN_MAX_MS;
    }
    if (m->join_attempts < 255) {
        m->join_attempts++;
    }
    return d + rnd % (d / 2u + 1u);
}

void lock_mac_check_sync(lock_mac_t *m, uint32_t now)
{
    if (m->sync == LOCK_MAC_SYNCED && now - m->last_sync_ms > LOCK_MAC_SYNC_TIMEOUT_MS) {
        m->sync = LOCK_MAC_UNSYNCED;
    }
}

uint32_t lock_mac_next_wakeup(const lock_mac_t *m, uint32_t now)
{
    if (m->sync == LOCK_MAC_UNSYNCED) {
        /* The gateway's wake-up train lasts longer than one period, so a
         * sniff once per period is guaranteed to catch one of its frames. */
        return now + m->slot_cfg.period_ms;
    }
    uint32_t start = slp_slot_next_start(&m->slot_cfg, now + SLP_DEFAULT_GUARD_MS, m->my_slot);
    return start - SLP_DEFAULT_GUARD_MS;
}

static bool in_my_groups(const lock_mac_t *m, uint16_t addr)
{
    for (uint8_t i = 0; i < m->group_count; i++) {
        if (m->groups[i] == addr) {
            return true;
        }
    }
    return false;
}

bool lock_mac_addressed_to_me(const lock_mac_t *m, const slp_frame_t *f)
{
    if (f->pan_id != m->pan_id || f->dst_mode != SLP_ADDR_SHORT) {
        return false;
    }
    uint16_t dst = (uint16_t)f->dst;
    return dst == m->short_addr || dst == SLP_SHORT_BROADCAST || in_my_groups(m, dst);
}

int lock_mac_replay_check(lock_mac_t *m, const slp_frame_t *f)
{
    if (!f->security) {
        return SLP_ERR_FORMAT; /* data to a lock is always secured */
    }
    uint16_t dst = (uint16_t)f->dst;
    slp_replay_t *r = (dst == m->short_addr) ? &m->gw_replay : &m->grp_replay;
    return slp_replay_check_and_update(r, f->frame_counter);
}

/* ---- uplink queue ---- */

void lock_upq_init(lock_upq_t *q)
{
    memset(q, 0, sizeof(*q));
}

bool lock_upq_push(lock_upq_t *q, const uint8_t *msg, size_t len)
{
    if (q->count == LOCK_UPQ_SIZE || len == 0 || len > LOCK_UPQ_MSG_MAX) {
        return false;
    }
    lock_upq_item_t *it = &q->items[(q->head + q->count) % LOCK_UPQ_SIZE];
    it->len = (uint8_t)len;
    memcpy(it->msg, msg, len);
    q->count++;
    return true;
}

static void pop(lock_upq_t *q)
{
    q->head = (uint8_t)((q->head + 1) % LOCK_UPQ_SIZE);
    q->count--;
    q->inflight = false;
    q->tries = 0;
    q->retry_at_ms = 0;
}

const lock_upq_item_t *lock_upq_due(const lock_upq_t *q, uint32_t now)
{
    if (q->count == 0) {
        return NULL;
    }
    if (q->inflight || (q->retry_at_ms != 0 && (int32_t)(now - q->retry_at_ms) < 0)) {
        return NULL;
    }
    return &q->items[q->head];
}

void lock_upq_sent(lock_upq_t *q, uint8_t seq, uint32_t now)
{
    q->inflight = true;
    q->seq = seq;
    if (q->tries > 0) {
        q->retries++;
    }
    q->tries++;
    q->sent++;
    uint32_t wait = LOCK_UPQ_RETRY_MS << (q->tries - 1u);
    q->retry_at_ms = now + wait;
    if (q->retry_at_ms == 0) {
        q->retry_at_ms = 1;
    }
}

bool lock_upq_on_ack(lock_upq_t *q, uint8_t seq)
{
    if (!q->inflight || seq != q->seq) {
        return false;
    }
    pop(q);
    return true;
}

bool lock_upq_check_timeout(lock_upq_t *q, uint32_t now)
{
    if (!q->inflight || (int32_t)(now - q->retry_at_ms) < 0) {
        return false;
    }
    if (q->tries >= LOCK_UPQ_TRIES) {
        q->dropped++;
        pop(q);
        return true;
    }
    q->inflight = false; /* due for a retry now */
    return false;
}
