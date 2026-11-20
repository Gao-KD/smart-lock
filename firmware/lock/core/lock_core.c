#include "lock_core.h"

#include "hal.h"

static bool is_user_source(lock_source_t src)
{
    return src != LOCK_SRC_REMOTE && src != LOCK_SRC_ADMIN;
}

/* Drive the clutch, retrying once. */
static bool drive(lock_core_t *c, bool engage)
{
    if (hal_clutch_drive(engage) == 0 || hal_clutch_drive(engage) == 0) {
        return true;
    }
    c->state = LOCK_ST_FAULT;
    hal_indicate(HAL_IND_ALARM);
    return false;
}

void lock_core_init(lock_core_t *c, uint16_t default_open_s)
{
    c->state = LOCK_ST_LOCKED;
    c->relock_at_ms = 0;
    c->default_open_s = default_open_s ? default_open_s : 5;
    c->double_locked = false;
}

slp_result_t lock_core_unlock(lock_core_t *c, lock_source_t src, uint16_t duration_s)
{
    if (c->state == LOCK_ST_EMERGENCY) {
        return SLP_RESULT_OK; /* already held open */
    }
    if (c->double_locked) {
        hal_indicate(HAL_IND_DENIED);
        return SLP_RESULT_DOUBLE_LOCKED;
    }
    if (c->state == LOCK_ST_LOCKDOWN && is_user_source(src)) {
        hal_indicate(HAL_IND_DENIED);
        return SLP_RESULT_DENIED;
    }
    if (!drive(c, true)) {
        return SLP_RESULT_HW_FAULT;
    }
    uint16_t s = duration_s ? duration_s : c->default_open_s;
    /* A remote unlock during lockdown opens once, then lockdown resumes. */
    if (c->state != LOCK_ST_LOCKDOWN) {
        c->state = LOCK_ST_UNLOCKED;
    }
    c->relock_at_ms = hal_millis() + (uint32_t)s * 1000u;
    if (c->relock_at_ms == 0) {
        c->relock_at_ms = 1; /* 0 means "no relock pending" */
    }
    hal_indicate(HAL_IND_OK);
    return SLP_RESULT_OK;
}

slp_result_t lock_core_lockdown(lock_core_t *c)
{
    if (!drive(c, false)) {
        return SLP_RESULT_HW_FAULT;
    }
    c->state = LOCK_ST_LOCKDOWN;
    c->relock_at_ms = 0;
    return SLP_RESULT_OK;
}

slp_result_t lock_core_emergency(lock_core_t *c)
{
    if (!drive(c, true)) {
        return SLP_RESULT_HW_FAULT;
    }
    c->state = LOCK_ST_EMERGENCY;
    c->relock_at_ms = 0;
    return SLP_RESULT_OK;
}

slp_result_t lock_core_release(lock_core_t *c)
{
    if (!drive(c, false)) {
        return SLP_RESULT_HW_FAULT;
    }
    c->state = LOCK_ST_LOCKED;
    c->relock_at_ms = 0;
    return SLP_RESULT_OK;
}

void lock_core_relock_now(lock_core_t *c)
{
    if (c->state == LOCK_ST_UNLOCKED && c->relock_at_ms != 0) {
        c->relock_at_ms = 0;
        if (drive(c, false)) {
            c->state = LOCK_ST_LOCKED;
        }
    }
}

void lock_core_tick(lock_core_t *c)
{
    if (c->relock_at_ms == 0) {
        return;
    }
    /* Wrap-safe "now >= relock_at". */
    if ((int32_t)(hal_millis() - c->relock_at_ms) < 0) {
        return;
    }
    c->relock_at_ms = 0;
    if (drive(c, false) && c->state == LOCK_ST_UNLOCKED) {
        c->state = LOCK_ST_LOCKED;
    }
}

const char *lock_state_name(lock_state_t s)
{
    switch (s) {
    case LOCK_ST_LOCKED:    return "LOCKED";
    case LOCK_ST_UNLOCKED:  return "UNLOCKED";
    case LOCK_ST_LOCKDOWN:  return "LOCKDOWN";
    case LOCK_ST_EMERGENCY: return "EMERGENCY";
    case LOCK_ST_FAULT:     return "FAULT";
    }
    return "?";
}
