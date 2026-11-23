/*
 * Lock application layer: executes decrypted end-to-end platform commands
 * and local events at the door (card, keypad, BLE phone, door sensor,
 * inside double lock, tamper switch, call button), keeps the record log,
 * and queues the uplink events the platform should hear about.
 *
 * Feature list and the reasoning behind each behaviour: docs/lock-features.md.
 */
#ifndef LOCK_APP_H
#define LOCK_APP_H

#include <stddef.h>
#include <stdint.h>

#include "credential.h"
#include "lock_core.h"
#include "offline_pwd.h"
#include "slp/security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4 x AA alkaline: 6.0 V new, ~1.1 V/cell = low, ~1.0 V/cell = critical. */
#define LOCK_BATT_LOW_MV      4400u
#define LOCK_BATT_CRITICAL_MV 4000u
/* Hysteresis so a level does not flap around a threshold under load. */
#define LOCK_BATT_HYST_MV     100u

/* Events waiting for the radio; the log ring keeps more for READ_LOG. */
#define LOCK_EVT_QUEUE 16u
#define LOCK_LOG_SIZE  128u

/* How long a visitor's unlock request stays answerable. */
#define LOCK_REQ_WINDOW_S 60u

/* BLE offline token: USER_ID(4) VALID_TO(4) FLAGS(1) MAC(8). */
#define LOCK_BLE_TOKEN_LEN 17u

typedef struct {
    uint16_t autolock_s;    /* open time before auto relock */
    uint8_t  volume;        /* 0 mute .. 3 */
    uint8_t  remote_allowed;/* 0: remote unlock only as answer to a request */
    uint8_t  wrong_limit;   /* failed attempts before lockout, 0 = off */
    uint16_t lockout_s;
    uint16_t heartbeat_s;
    uint16_t door_alarm_s;  /* door-left-open alarm, 0 = off */
    int16_t  tz_offset_min; /* local time for weekly schedules (China: +480) */
} lock_cfg_t;

typedef struct {
    uint8_t  type;        /* SLP_EVT_ACCESS / ALARM / BATTERY / DOOR / UNLOCK_REQ */
    uint8_t  result;      /* access: slp_result_t */
    uint8_t  cred_type;
    uint8_t  source;      /* access: lock_source_t */
    uint8_t  alarm;       /* slp_alarm_t */
    uint8_t  door;
    uint8_t  double_lock;
    uint8_t  replayed;
    uint8_t  id_len;
    uint8_t  id[CRED_ID_MAX];
    uint16_t req_id;
    uint16_t battery_mv;
    uint8_t  batt_level;  /* slp_batt_level_t */
    uint32_t user_id;
    uint32_t at_unix;
    uint32_t log_seq;
} lock_event_t;

typedef struct {
    lock_core_t       core;
    lock_cfg_t        cfg;
    cred_store_t      creds;
    ofp_state_t       ofp;
    slp_msgid_cache_t dedup;
    uint8_t           batt_level;
    /* uplink queue */
    lock_event_t      evq[LOCK_EVT_QUEUE];
    uint8_t           evq_head;
    uint8_t           evq_count;
    uint32_t          evt_dropped;
    uint32_t          evt_seq;
    /* record log (NVM ring on EFR32) */
    lock_event_t      log[LOCK_LOG_SIZE];
    uint32_t          log_seq;      /* seq of the newest record, 0 = none */
    /* door and security */
    bool              door_open;
    uint32_t          door_open_ms;
    bool              door_alarmed;
    uint8_t           wrong_count;
    uint32_t          locked_out_until_ms; /* 0 = not locked out */
    /* visitor unlock request */
    uint16_t          req_id;
    uint32_t          req_until_ms;        /* 0 = none pending */
    uint32_t          clock_fixes;         /* beacon time corrections */
} lock_app_t;

void lock_app_init(lock_app_t *a);

/* Executes one plaintext app message (after the end-to-end MIC check) and
 * writes the SLP_RSP_RESULT message into rsp. Returns the response length,
 * or 0 if no response can be built. */
size_t lock_app_handle_command(lock_app_t *a, const uint8_t *req, size_t req_len,
                               uint8_t *rsp, size_t rsp_cap);

/* ---- local events at the door ---- */

/* Card / fingerprint / face presented. Queues an SLP_EVT_ACCESS record. */
slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len);
/* Digits typed on the keypad then '#': stored passwords (decoy digits
 * allowed) and offline passwords. */
slp_result_t lock_app_keypad(lock_app_t *a, const uint8_t *digits, uint8_t len);
/* Phone at the door over BLE presenting a platform-issued token (works with
 * no gateway at all). */
slp_result_t lock_app_ble(lock_app_t *a, const uint8_t *token, uint8_t len);
void lock_app_door(lock_app_t *a, bool open);
void lock_app_double_lock(lock_app_t *a, bool on);
/* The radio's fake-wake-up guard reduced the sniff rate (battery drain attack). */
void lock_app_rf_jam(lock_app_t *a);
void lock_app_tamper(lock_app_t *a);
void lock_app_mech_key(lock_app_t *a);
/* Visitor pressed the call button: returns the request id. */
uint16_t lock_app_doorbell(lock_app_t *a);

/* Call on every wake-up: auto relock, lockout expiry, door-open alarm. */
void lock_app_tick(lock_app_t *a);

/* Wall clock from the gateway beacon. Corrects drift above 2 s; returns the
 * applied correction in seconds (0 = none). */
int32_t lock_app_time_sync(lock_app_t *a, uint32_t unix_s);

/* Samples the battery; queues SLP_EVT_BATTERY when the level changes. */
slp_batt_level_t lock_app_battery_check(lock_app_t *a);

/* Pops the oldest queued event as a plaintext app message. Returns its
 * length, or 0 when the queue is empty. */
size_t lock_app_pop_event(lock_app_t *a, uint8_t *buf, size_t cap);

/* Builds the SLP_EVT_HEARTBEAT message. */
size_t lock_app_heartbeat(lock_app_t *a, uint8_t *buf, size_t cap, slp_hb_reason_t reason);
/* Next heartbeat interval in ms: configured interval, x4 on critical
 * battery, with +-10 % jitter from `rnd` so a building does not report in
 * lock step. */
uint32_t lock_app_heartbeat_ms(const lock_app_t *a, uint32_t rnd);

/* Status flags (SLP_LF_*). */
uint8_t lock_app_flags(const lock_app_t *a);
/* Minutes since Monday 00:00, local time. */
uint16_t lock_app_local_min_of_week(const lock_app_t *a, uint32_t unix_s);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_APP_H */
