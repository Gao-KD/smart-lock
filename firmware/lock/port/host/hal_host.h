/* Host (simulated) HAL: lets tests drive time and inspect actuator calls. */
#ifndef LOCK_HAL_HOST_H
#define LOCK_HAL_HOST_H

#include "hal.h"

typedef struct {
    uint32_t millis;
    uint32_t unix_time;
    bool     clutch_engaged;
    int      clutch_calls;
    int      clutch_fail_next;   /* >0: the next N clutch drives fail */
    hal_indication_t last_ind;
    int      ind_calls;
    uint16_t battery_mv;
} hal_host_state_t;

extern hal_host_state_t hal_host;

void hal_host_reset(void);
void hal_host_advance(uint32_t ms);

#endif
