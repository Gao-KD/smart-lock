/*
 * Phase-synchronised periodic listening with synchronous wake-up
 * (docs/radio-plan.md).
 *
 * The listen period (default 1000 ms) is divided into slots (default 20 ms).
 * Each lock sniffs for a preamble once per period at the start of its
 * assigned slot. The lock learns the period epoch from the gateway's ACKs
 * (no beacons), so the gateway can transmit when a given lock is listening:
 * it starts a preamble early enough to cover both clocks' drift since that
 * ACK, then sends the frame.
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

/* ---- synchronous wake-up ---- */
/* Preamble sniff duration per wake-up (RAIL preamble detect). */
#define SLP_SNIFF_MS             1u
/* Worst-case relative drift of lock + gateway clocks (2 x 20 ppm). */
#define SLP_DRIFT_PPM            40u
/* Fixed margin added to the drift-based preamble (sniff + radio turnaround). */
#define SLP_SYNC_PREAMBLE_MIN_MS 8u
/* Longer than this and a wake-up train is used instead (~50 min of drift). */
#define SLP_SYNC_PREAMBLE_MAX_MS 250u
/* Asynchronous wake-up train: longer than a period, so any lock sniffing
 * once per period hears one of its wake-up frames. */
#define SLP_WAKE_TRAIN_EXTRA_MS  50u

/* Preamble length that guarantees a lock whose last sync was `since_sync_ms`
 * ago hears a synchronous transmission: the preamble starts drift before
 * the slot start and lasts until drift after it. Returns 0 when the drift is
 * too large and a wake-up train must be used. */
static inline uint16_t slp_sync_preamble_ms(uint32_t since_sync_ms)
{
    uint64_t drift = ((uint64_t)since_sync_ms * SLP_DRIFT_PPM + 999999u) / 1000000u;
    uint64_t pre = 2u * drift + SLP_SYNC_PREAMBLE_MIN_MS;
    return pre > SLP_SYNC_PREAMBLE_MAX_MS ? 0u : (uint16_t)pre;
}

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

/* Re-anchor the epoch when the gateway says "slot `slot` started at `rx_time`". */
void slp_slot_resync(slp_slot_cfg_t *c, uint32_t rx_time, uint16_t slot);

#ifdef __cplusplus
}
#endif

#endif /* SLP_SLOT_H */
