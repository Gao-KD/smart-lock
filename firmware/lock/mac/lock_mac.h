/*
 * Lock-side MAC: synchronous wake-up listening, address filtering,
 * anti-replay, reliable uplink and the fake-wake-up (battery drain) guard.
 * Radio access (RAIL) lives in port/efr32; this module only decides *when*
 * to sniff and *whether* to accept a frame. Design: docs/radio-plan.md.
 *
 * There is no gateway beacon. Every lock owns a (channel, slot) and sniffs
 * for a preamble for ~1 ms at its slot start once per period. The slot
 * phase, the period number and the wall clock are learnt from the
 * authenticated ACK that answers each uplink (and from Join-Accept). The
 * gateway transmits only when it has data: a preamble long enough to cover
 * the clock drift since that last ACK, then the frame. A lock whose phase
 * is unknown is reached with a ~1 s wake-up train, which any once-per-period
 * sniff catches.
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

/* Without an ACK for this long the phase is no longer trusted (the drift
 * exceeds the longest synchronous preamble, SLP_SYNC_PREAMBLE_MAX_MS): only
 * wake-up trains reach the lock. Heartbeats (default 30 min) keep it fresh. */
#define LOCK_MAC_SYNC_TIMEOUT_MS 3000000u
/* After an uplink went unanswered the link is "suspect"; the first ACK after
 * that ends the connection loss and the caller sends a RECONNECT heartbeat
 * (the platform then re-sends parked commands and pulls missed records). */
/* Suspect and no ACK for this long: the gateway is gone (rebooted with a new
 * table, or replaced). Join again, with exponential back-off. */
#define LOCK_MAC_REJOIN_MS 600000u
#define LOCK_MAC_JOIN_MIN_MS 3000u
#define LOCK_MAC_JOIN_MAX_MS 300000u
/* While suspect with nothing queued, probe with a heartbeat: 5 s, 10 s ...
 * capped at 5 min. */
#define LOCK_MAC_PROBE_MIN_MS 5000u
#define LOCK_MAC_PROBE_MAX_MS 300000u

/* ---- fake wake-up guard ----
 * A preamble that does not lead to a valid frame for this lock (no sync, bad
 * CRC, other PAN, MIC failure) is a false wake-up. An attacker sending
 * preambles all day would keep the receiver on and drain the battery, so
 * after LOCK_MAC_DRAIN_WAKES false wake-ups within LOCK_MAC_DRAIN_WINDOW_MS
 * the lock sniffs only every 2nd period, then 4th, up to every
 * LOCK_MAC_DIV_MAX-th, and tells the gateway (MAC command SNIFF_DIV) so it
 * schedules downlinks only in periods the lock listens to. After
 * LOCK_MAC_DRAIN_QUIET_MS without a false wake-up the rate doubles again. */
#define LOCK_MAC_DRAIN_WAKES     10u
#define LOCK_MAC_DRAIN_WINDOW_MS 60000u
#define LOCK_MAC_DRAIN_QUIET_MS  300000u
#define LOCK_MAC_DIV_MAX         8u

/* MAC command ids (payload byte 0 of SLP_FT_MAC_CMD frames). */
enum {
    LOCK_MAC_CMD_JOIN_REQ    = 0x01, /* lock -> gw, unsecured: (empty) */
    LOCK_MAC_CMD_JOIN_ACCEPT = 0x02, /* gw -> lock: short u16 | ch u8 | slot u16 | period_ms u16 | slot_ms u16 | phase_ms u16 | period u32 */
    LOCK_MAC_CMD_PROBE       = 0x03, /* gw -> lock, secured, ACK requested: answer with a RECONNECT heartbeat */
    LOCK_MAC_CMD_SNIFF_DIV   = 0x04  /* lock -> gw, secured: div u8 */
};

/* Payload of the secured ACK frame from the gateway. */
#define LOCK_MAC_ACK_PAYLOAD_LEN 10u /* phase_ms u16 | period u32 | unix u32 */

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
    uint32_t        last_sync_ms;   /* last authenticated ACK / Join-Accept */
    uint32_t        epoch_period;   /* gateway period number starting at slot_cfg.epoch_ms */
    slp_replay_t    gw_replay;   /* unicast frames and ACKs from the gateway */
    slp_replay_t    grp_replay;  /* broadcast / group frames */
    bool            suspect;     /* an uplink went unanswered */
    uint32_t        suspect_since_ms;
    uint32_t        lost_count;  /* connection losses that ended */
    uint8_t         join_attempts;
    uint8_t         probe_attempts;
    /* fake wake-up guard */
    uint8_t         sniff_div;   /* 1, 2, 4, 8 */
    uint8_t         false_wakes; /* in the current window */
    uint32_t        false_window_ms;
    uint32_t        last_false_ms;
    uint32_t        false_total;
} lock_mac_t;

/* Parameters come from Join-Accept: the gateway's current period number
 * and how far into it (phase_ms) the accept was sent, received at rx_ms. */
void lock_mac_init(lock_mac_t *m, uint16_t pan_id, uint16_t short_addr, uint8_t channel,
                   uint16_t slot, uint16_t period_ms, uint16_t slot_ms);
void lock_mac_on_join_accept(lock_mac_t *m, uint32_t rx_ms, uint16_t phase_ms, uint32_t period);

/* An authenticated ACK arrived at rx_ms: re-anchor phase and period number.
 * Returns true when it ends a connection loss (the link was suspect): the
 * caller sends a RECONNECT heartbeat. */
bool lock_mac_on_ack(lock_mac_t *m, uint32_t rx_ms, uint16_t phase_ms, uint32_t period);

/* An uplink was retried or dropped without ACK. */
void lock_mac_uplink_failed(lock_mac_t *m, uint32_t now);

/* True when the gateway has been silent so long that the lock must join
 * again. */
bool lock_mac_need_rejoin(const lock_mac_t *m, uint32_t now);

/* Delay before the next join request: 3 s, 6 s, 12 s ... 5 min, plus up to
 * 50 % random jitter so a building that lost its gateway does not rejoin
 * in one burst. Increments the attempt counter. */
uint32_t lock_mac_join_backoff(lock_mac_t *m, uint32_t rnd);

/* Delay before the next probe heartbeat while suspect. */
uint32_t lock_mac_probe_delay(lock_mac_t *m, uint32_t rnd);

/* Drops to UNSYNCED when no ACK came for too long; relaxes the sniff
 * divisor after a quiet time. Returns true if sniff_div changed. */
bool lock_mac_check_sync(lock_mac_t *m, uint32_t now);

/* Gateway period number running at `now` (valid once synced). */
uint32_t lock_mac_period_at(const lock_mac_t *m, uint32_t now);

/* Whether the lock sniffs in gateway period `period` (sniff_div). */
bool lock_mac_sniff_period(const lock_mac_t *m, uint32_t period);

/* When the radio should next wake up and sniff. */
uint32_t lock_mac_next_wakeup(const lock_mac_t *m, uint32_t now);

/* A sniff detected a preamble that did not lead to a valid frame for us.
 * Returns true if sniff_div changed (send SNIFF_DIV, raise an alarm). */
bool lock_mac_on_false_wake(lock_mac_t *m, uint32_t now);

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
