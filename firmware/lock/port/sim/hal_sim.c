#define _POSIX_C_SOURCE 200809L
#include "hal_sim.h"

#include <time.h>

#include "slp/secure.h"

hal_sim_lock_t *hal_sim_cur;

static struct timespec t0;
static int t0_set;

uint32_t hal_sim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (!t0_set) {
        t0 = ts;
        t0_set = 1;
    }
    return (uint32_t)((ts.tv_sec - t0.tv_sec) * 1000 + (ts.tv_nsec - t0.tv_nsec) / 1000000);
}

uint32_t hal_millis(void) { return hal_sim_now_ms(); }

uint32_t hal_unix_time(void)
{
    return (uint32_t)((int64_t)time(NULL) + (hal_sim_cur ? hal_sim_cur->unix_offset : 0));
}

void hal_set_unix_time(uint32_t unix_s)
{
    if (hal_sim_cur) {
        hal_sim_cur->unix_offset = (int32_t)((int64_t)unix_s - (int64_t)time(NULL));
    }
}

uint16_t hal_battery_mv(void) { return hal_sim_cur ? hal_sim_cur->battery_mv : 6000; }

int hal_clutch_drive(bool engage)
{
    if (hal_sim_cur->clutch_fail_next > 0) {
        hal_sim_cur->clutch_fail_next--;
        return -1;
    }
    hal_sim_cur->clutch_engaged = engage;
    return 0;
}

void hal_indicate(hal_indication_t ind)
{
    hal_sim_cur->last_ind = ind;
    hal_sim_cur->last_ind_ms = hal_sim_now_ms();
    hal_sim_cur->ind_calls++;
}

/* Credentials live in RAM for the simulator (NVM3 on EFR32). */
int hal_nvm_write(uint16_t key, const void *data, size_t len)
{
    (void)key; (void)data; (void)len;
    return 0;
}

int hal_nvm_read(uint16_t key, void *data, size_t len)
{
    (void)key; (void)data; (void)len;
    return -1;
}

int hal_app_mac(const uint8_t *msg, size_t len, uint8_t out[8])
{
    return slp_app_mac(hal_sim_cur->app_key, hal_sim_cur->eui, msg, len, out);
}
