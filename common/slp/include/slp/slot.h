/*
 * Phase-synchronised periodic listening (design doc: "时间结构").
 *
 * The listen period (default 1000 ms) is divided into slots (default 20 ms).
 * Each lock listens once per period at the start of its assigned slot. Both
 * sides share the period epoch, learnt from gateway beacons, so the gateway
 * can transmit exactly when a given lock is listening.
 *
 * All times are milliseconds on a free-running 32-bit clock (wrap-safe).
 */
#ifndef SLP_SLOT_H
#define SLP_SLOT_H

#include "slp/slp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLP_DEFAULT_PERIOD_MS 1000u
#define SLP_DEFAULT_SLOT_MS   20u
/* Listen window guard on each side of the slot start (crystal drift). */
#define SLP_DEFAULT_GUARD_MS  2u

typedef struct {
    uint32_t epoch_ms;   /* local time at which slot 0 of some period started */
    uint16_t period_ms;
    uint16_t slot_ms;
} slp_slot_cfg_t;

static inline uint16_t slp_slots_per_period(const slp_slot_cfg_t *c)
{
    return (uint16_t)(c->period_ms / c->slot_ms);
}

/* Start time of the next occurrence of `slot` strictly after `now`
 * (or equal to now). */
uint32_t slp_slot_next_start(const slp_slot_cfg_t *c, uint32_t now, uint16_t slot);

/* Slot index currently running at `now`. */
uint16_t slp_slot_current(const slp_slot_cfg_t *c, uint32_t now);

/* Re-anchor the epoch when a beacon says "slot `slot` started at `rx_time`". */
void slp_slot_resync(slp_slot_cfg_t *c, uint32_t rx_time, uint16_t slot);

#ifdef __cplusplus
}
#endif

#endif /* SLP_SLOT_H */
