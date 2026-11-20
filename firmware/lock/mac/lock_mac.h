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
/* Beacons missing for this long count as "connection lost": the first
 * beacon after that triggers a RECONNECT heartbeat (the platform then
 * re-sends parked commands and pulls missed records). */
#define LOCK_MAC_LOST_MS 5000u
/* Without any beacon for this long the gateway is gone (rebooted with a new
 * table, or replaced): join again, with exponential back-off. */
#define LOCK_MAC_REJOIN_MS 600000u
#define LOCK_MAC_JOIN_MIN_MS 3000u
#define LOCK_MAC_JOIN_MAX_MS 300000u

/* ---- reliable uplink ----
 * Every uplink (result, event, heartbeat) is a DATA frame with ACK request.
 * The gateway answers with an ACK frame echoing SEQ right after the frame
 * (the lock keeps RX on for a few ms). Without an ACK the lock retries
 * with back-off: 300 ms, 600 ms, 1.2 s, ... up to LOCK_UPQ_TRIES, then
 * drops the message. Events are not lost even then: they stay in the
 * record log and the platform pulls gaps in LOG_SEQ with READ_LOG. */
#define LOCK_UPQ_SIZE     16u
#define LOCK_UPQ_MSG_MAX  96u
#define LOCK_UPQ_TRIES    6u
#define LOCK_UPQ_RETRY_MS 300u

typedef enum { LOCK_MAC_UNSYNCED = 0, LOCK_MAC_SYNCED } lock_mac_sync_t;

typedef struct {
    uint8_t len;
    uint8_t msg[LOCK_UPQ_MSG_MAX];
} lock_upq_item_t;

typedef struct {
    lock_upq_item_t items[LOCK_UPQ_SIZE];
    uint8_t  head, count;
    bool     inflight;      /* head transmitted, waiting for its ACK */
    uint8_t  seq;           /* SEQ of the in-flight frame */
    uint8_t  tries;
    uint32_t retry_at_ms;
    uint32_t sent, retries, dropped;
} lock_upq_t;

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
    uint32_t        lost_count;  /* times the beacon was lost and came back */
    uint8_t         join_attempts;
} lock_mac_t;

/* Parameters come from Join-Accept. */
void lock_mac_init(lock_mac_t *m, uint16_t pan_id, uint16_t short_addr, uint8_t channel,
                   uint16_t slot, uint16_t period_ms, uint16_t slot_ms);

/* A beacon received at rx_ms announces that `beacon_slot` started then.
 * Returns true when it ends a connection loss (no beacon for
 * LOCK_MAC_LOST_MS): the caller sends a RECONNECT heartbeat. */
bool lock_mac_on_beacon(lock_mac_t *m, uint32_t rx_ms, uint16_t beacon_slot);

/* True when the gateway has been silent so long that the lock must join
 * again. */
bool lock_mac_need_rejoin(const lock_mac_t *m, uint32_t now);

/* Delay before the next join request: 3 s, 6 s, 12 s ... 5 min, plus up to
 * 50 % random jitter so a building that lost its gateway does not rejoin
 * in one burst. Increments the attempt counter. */
uint32_t lock_mac_join_backoff(lock_mac_t *m, uint32_t rnd);

/* Drops to UNSYNCED when beacons have been missing too long. */
void lock_mac_check_sync(lock_mac_t *m, uint32_t now);

/* When the radio should next wake up and sniff. */
uint32_t lock_mac_next_wakeup(const lock_mac_t *m, uint32_t now);

/* Header checks before decryption: PAN, destination (own short address,
 * one of our groups, or broadcast). */
bool lock_mac_addressed_to_me(const lock_mac_t *m, const slp_frame_t *f);

/* After a successful MIC check: enforce the strictly increasing counter. */
int lock_mac_replay_check(lock_mac_t *m, const slp_frame_t *f);

/* ---- uplink queue ---- */
void lock_upq_init(lock_upq_t *q);
/* Copies a plaintext app message into the queue. False when full. */
bool lock_upq_push(lock_upq_t *q, const uint8_t *msg, size_t len);
static inline uint8_t lock_upq_free(const lock_upq_t *q) { return (uint8_t)(LOCK_UPQ_SIZE - q->count); }
/* The message to (re)transmit now, or NULL when nothing is due. */
const lock_upq_item_t *lock_upq_due(const lock_upq_t *q, uint32_t now);
/* The caller transmitted the head with frame SEQ `seq`. */
void lock_upq_sent(lock_upq_t *q, uint8_t seq, uint32_t now);
/* An ACK frame arrived. True if it acknowledged the head (removed). */
bool lock_upq_on_ack(lock_upq_t *q, uint8_t seq);
/* No ACK before the retry time: schedules a retry or drops the head. Call
 * before lock_upq_due. Returns true if the head was dropped. */
bool lock_upq_check_timeout(lock_upq_t *q, uint32_t now);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_MAC_H */
