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

/* hal_app_mac uses crypto_host key handle HAL_HOST_APP_KEY (tests set it
 * with slp_host_key_set) and this EUI. */
#define HAL_HOST_APP_KEY 900u
#define HAL_HOST_EUI     0x5A00000000010101ull

void hal_host_reset(void);
void hal_host_advance(uint32_t ms);

#endif
