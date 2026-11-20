#include "slp/slot.h"

uint16_t slp_slot_current(const slp_slot_cfg_t *c, uint32_t now)
{
    uint32_t elapsed = now - c->epoch_ms; /* wrap-safe */
    return (uint16_t)((elapsed % c->period_ms) / c->slot_ms);
}

uint32_t slp_slot_next_start(const slp_slot_cfg_t *c, uint32_t now, uint16_t slot)
{
    uint32_t elapsed = now - c->epoch_ms;
    uint32_t in_period = elapsed % c->period_ms;
    uint32_t period_start = now - in_period;
    uint32_t offset = (uint32_t)slot * c->slot_ms;
    if (offset >= in_period) {
        return period_start + offset;
    }
    return period_start + c->period_ms + offset;
}

void slp_slot_resync(slp_slot_cfg_t *c, uint32_t rx_time, uint16_t slot)
{
    c->epoch_ms = rx_time - (uint32_t)slot * c->slot_ms;
}
