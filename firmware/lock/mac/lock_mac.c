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

void lock_mac_on_beacon(lock_mac_t *m, uint32_t rx_ms, uint16_t beacon_slot)
{
    slp_slot_resync(&m->slot_cfg, rx_ms, beacon_slot);
    m->sync = LOCK_MAC_SYNCED;
    m->last_sync_ms = rx_ms;
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
