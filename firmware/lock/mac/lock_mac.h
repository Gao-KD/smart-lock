/*
 * Lock-side MAC: phase-synchronised listening, address filtering and
 * anti-replay. Radio access (RAIL) lives in port/efr32; this module only
 * decides *when* to listen and *whether* to accept a frame.
 */
#ifndef LOCK_MAC_H
#define LOCK_MAC_H

#include <stdbool.h>
#include <stdint.h>

#include "slp/frame.h"
#include "slp/security.h"
#include "slp/slot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Without a beacon for this long the lock assumes its clock drifted out of
 * the guard window and falls back to asynchronous wake-up-train listening. */
#define LOCK_MAC_SYNC_TIMEOUT_MS 120000u

typedef enum { LOCK_MAC_UNSYNCED = 0, LOCK_MAC_SYNCED } lock_mac_sync_t;

typedef struct {
    slp_slot_cfg_t  slot_cfg;
    uint16_t        my_slot;
    uint8_t         channel;
    uint16_t        pan_id;
    uint16_t        short_addr;
    uint16_t        groups[8];
    uint8_t         group_count;
    lock_mac_sync_t sync;
    uint32_t        last_sync_ms;
    slp_replay_t    gw_replay;   /* unicast frames from the gateway */
    slp_replay_t    grp_replay;  /* broadcast / group frames */
} lock_mac_t;

/* Parameters come from Join-Accept. */
void lock_mac_init(lock_mac_t *m, uint16_t pan_id, uint16_t short_addr, uint8_t channel,
                   uint16_t slot, uint16_t period_ms, uint16_t slot_ms);

/* A beacon received at rx_ms announces that `beacon_slot` started then. */
void lock_mac_on_beacon(lock_mac_t *m, uint32_t rx_ms, uint16_t beacon_slot);

/* Drops to UNSYNCED when beacons have been missing too long. */
void lock_mac_check_sync(lock_mac_t *m, uint32_t now);

/* When the radio should next wake up and sniff. */
uint32_t lock_mac_next_wakeup(const lock_mac_t *m, uint32_t now);

/* Header checks before decryption: PAN, destination (own short address,
 * one of our groups, or broadcast). */
bool lock_mac_addressed_to_me(const lock_mac_t *m, const slp_frame_t *f);

/* After a successful MIC check: enforce the strictly increasing counter. */
int lock_mac_replay_check(lock_mac_t *m, const slp_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_MAC_H */
