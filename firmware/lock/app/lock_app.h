/*
 * Lock application layer: executes decrypted end-to-end platform commands
 * and local access attempts (card, password, ...), and queues the uplink
 * events (access log, battery) the platform should hear about.
 */
#ifndef LOCK_APP_H
#define LOCK_APP_H

#include <stddef.h>
#include <stdint.h>

#include "credential.h"
#include "lock_core.h"
#include "slp/security.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 4 x AA alkaline: 6.0 V new, ~1.1 V/cell = low, ~1.0 V/cell = critical. */
#define LOCK_BATT_LOW_MV      4400u
#define LOCK_BATT_CRITICAL_MV 4000u
/* Hysteresis so a level does not flap around a threshold under load. */
#define LOCK_BATT_HYST_MV     100u

#define LOCK_EVT_QUEUE 8u

typedef struct {
    uint8_t  type;        /* SLP_EVT_ACCESS or SLP_EVT_BATTERY */
    uint8_t  result;      /* access: slp_result_t */
    uint8_t  cred_type;
    uint8_t  id_len;
    uint8_t  id[CRED_ID_MAX];
    uint32_t user_id;
    uint32_t at_unix;
    uint16_t battery_mv;
    uint8_t  batt_level;  /* slp_batt_level_t */
} lock_event_t;

typedef struct {
    lock_core_t       core;
    cred_store_t      creds;
    slp_msgid_cache_t dedup;
    uint8_t           batt_level;
    lock_event_t      evq[LOCK_EVT_QUEUE];
    uint8_t           evq_head;
    uint8_t           evq_count;
    uint32_t          evt_dropped;
    uint32_t          evt_seq;
} lock_app_t;

void lock_app_init(lock_app_t *a);

/* Executes one plaintext app message (after the end-to-end MIC check) and
 * writes the SLP_RSP_RESULT message into rsp. Returns the response length,
 * or 0 if no response can be built. */
size_t lock_app_handle_command(lock_app_t *a, const uint8_t *req, size_t req_len,
                               uint8_t *rsp, size_t rsp_cap);

/* Local access with a credential presented at the door. Queues an
 * SLP_EVT_ACCESS event (granted or denied). */
slp_result_t lock_app_access(lock_app_t *a, uint8_t cred_type, const uint8_t *id, uint8_t id_len);

/* Samples the battery; queues SLP_EVT_BATTERY when the level changes.
 * Returns the current level. Call on every wake-up that drives the motor
 * and from the heartbeat. */
slp_batt_level_t lock_app_battery_check(lock_app_t *a);

/* Pops the oldest queued event as a plaintext app message. Returns its
 * length, or 0 when the queue is empty. */
size_t lock_app_pop_event(lock_app_t *a, uint8_t *buf, size_t cap);

/* Builds the periodic SLP_EVT_HEARTBEAT message. */
size_t lock_app_heartbeat(lock_app_t *a, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_APP_H */
