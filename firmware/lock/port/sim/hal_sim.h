/*
 * Multi-instance HAL for the PC simulator: one process hosts hundreds of
 * virtual locks running the real firmware logic (lock_app / lock_core /
 * lock_mac). Before calling into a lock, set hal_sim_cur to that lock's
 * hardware state.
 */
#ifndef LOCK_HAL_SIM_H
#define LOCK_HAL_SIM_H

#include "hal.h"

typedef struct {
    int32_t          unix_offset;     /* TIME_SYNC moves this lock's wall clock */
    bool             clutch_engaged;  /* motor position: true = handle drives latch */
    int              clutch_fail_next;/* >0: next N drives stall (jammed motor) */
    hal_indication_t last_ind;
    uint32_t         last_ind_ms;
    int              ind_calls;
    uint16_t         battery_mv;
} hal_sim_lock_t;

extern hal_sim_lock_t *hal_sim_cur;

/* Monotonic milliseconds since the simulator started. */
uint32_t hal_sim_now_ms(void);

#endif
