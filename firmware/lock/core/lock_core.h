/*
 * Lock state machine.
 *
 *   LOCKED --unlock--> UNLOCKED --timeout--> LOCKED
 *   any    --lockdown--> LOCKDOWN (user credentials refused) --release--> LOCKED
 *   any    --emergency--> EMERGENCY (held open)              --release--> LOCKED
 *   clutch failure twice in a row --> FAULT (reported, next command retries)
 *
 * The inside thumb-turn of the 24x240 mortise throws the deadbolt ("反锁",
 * double lock). The panel only sees it through a micro switch: while it is
 * thrown no outside unlock can work, so every unlock answers DOUBLE_LOCKED
 * (the mechanical override key still works, it is not driven by us).
 */
#ifndef LOCK_CORE_H
#define LOCK_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "slp/app.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LOCK_ST_LOCKED = 0,
    LOCK_ST_UNLOCKED,
    LOCK_ST_LOCKDOWN,
    LOCK_ST_EMERGENCY,
    LOCK_ST_FAULT
} lock_state_t;

typedef enum {
    LOCK_SRC_CARD = 0,
    LOCK_SRC_PASSWORD,
    LOCK_SRC_FINGERPRINT,
    LOCK_SRC_FACE,
    LOCK_SRC_BLE,
    LOCK_SRC_REMOTE,   /* platform command over Sub-GHz */
    LOCK_SRC_ADMIN     /* maintenance over BLE with admin token */
} lock_source_t;

typedef struct {
    lock_state_t state;
    uint32_t relock_at_ms;
    uint16_t default_open_s;
    bool     double_locked;
} lock_core_t;

void lock_core_init(lock_core_t *c, uint16_t default_open_s);

/* duration_s = 0 uses the default. */
slp_result_t lock_core_unlock(lock_core_t *c, lock_source_t src, uint16_t duration_s);
slp_result_t lock_core_lockdown(lock_core_t *c);
slp_result_t lock_core_emergency(lock_core_t *c);
slp_result_t lock_core_release(lock_core_t *c);

/* Relocks right away (door closed again while UNLOCKED). */
void lock_core_relock_now(lock_core_t *c);

/* Call periodically (and on every wake-up) to handle auto-relock. */
void lock_core_tick(lock_core_t *c);

const char *lock_state_name(lock_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_CORE_H */
