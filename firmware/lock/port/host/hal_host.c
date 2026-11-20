#include "hal_host.h"

#include <string.h>

#include "slp/secure.h"

hal_host_state_t hal_host;

#define NVM_SLOTS 16
#define NVM_MAX   64
static struct {
    uint16_t key;
    size_t   len;
    uint8_t  data[NVM_MAX];
    bool     used;
} nvm[NVM_SLOTS];

void hal_host_reset(void)
{
    memset(&hal_host, 0, sizeof(hal_host));
    hal_host.unix_time = 1800000000u;
    hal_host.battery_mv = 6000;
    memset(nvm, 0, sizeof(nvm));
}

void hal_host_advance(uint32_t ms)
{
    uint32_t before = hal_host.millis;
    hal_host.millis += ms;
    hal_host.unix_time += (hal_host.millis / 1000u) - (before / 1000u);
}

uint32_t hal_millis(void) { return hal_host.millis; }
uint32_t hal_unix_time(void) { return hal_host.unix_time; }
void hal_set_unix_time(uint32_t unix_s) { hal_host.unix_time = unix_s; }
uint16_t hal_battery_mv(void) { return hal_host.battery_mv; }

int hal_clutch_drive(bool engage)
{
    hal_host.clutch_calls++;
    if (hal_host.clutch_fail_next > 0) {
        hal_host.clutch_fail_next--;
        return -1;
    }
    hal_host.clutch_engaged = engage;
    return 0;
}

void hal_indicate(hal_indication_t ind)
{
    hal_host.last_ind = ind;
    hal_host.ind_calls++;
}

int hal_nvm_write(uint16_t key, const void *data, size_t len)
{
    if (len > NVM_MAX) {
        return -1;
    }
    for (int i = 0; i < NVM_SLOTS; i++) {
        if (!nvm[i].used || nvm[i].key == key) {
            nvm[i].used = true;
            nvm[i].key = key;
            nvm[i].len = len;
            memcpy(nvm[i].data, data, len);
            return 0;
        }
    }
    return -1;
}

int hal_nvm_read(uint16_t key, void *data, size_t len)
{
    for (int i = 0; i < NVM_SLOTS; i++) {
        if (nvm[i].used && nvm[i].key == key) {
            size_t n = len < nvm[i].len ? len : nvm[i].len;
            memcpy(data, nvm[i].data, n);
            return (int)n;
        }
    }
    return -1;
}

int hal_app_mac(const uint8_t *msg, size_t len, uint8_t out[8])
{
    return slp_app_mac(HAL_HOST_APP_KEY, HAL_HOST_EUI, msg, len, out);
}
