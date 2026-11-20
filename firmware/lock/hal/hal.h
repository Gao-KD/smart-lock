/*
 * Hardware abstraction for the lock firmware.
 *
 * Implementations:
 *   port/host   - simulated hardware for unit tests and the host demo
 *   port/efr32  - EFR32FG28 (Simplicity SDK: RAIL, PSA Crypto, NVM3, EMU)
 */
#ifndef LOCK_HAL_H
#define LOCK_HAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Free-running millisecond clock (wraps at 2^32). */
uint32_t hal_millis(void);

/* Wall clock, Unix seconds, kept in sync by gateway beacons / TIME_SYNC. */
uint32_t hal_unix_time(void);
void hal_set_unix_time(uint32_t unix_s);

/* Clutch motor in the panel (standard 24x240 mortise is mechanical):
 * engage = outer handle drives the latch. Returns 0, or <0 on stall/fault. */
int hal_clutch_drive(bool engage);

typedef enum {
    HAL_IND_OK,        /* green + short beep */
    HAL_IND_DENIED,    /* red + long beep */
    HAL_IND_LOW_BATT,
    HAL_IND_ALARM
} hal_indication_t;
void hal_indicate(hal_indication_t ind);

/* Battery voltage in millivolts (divider enabled only while sampling). */
uint16_t hal_battery_mv(void);

/* Key-value persistent storage (NVM3 on EFR32). */
int hal_nvm_write(uint16_t key, const void *data, size_t len);
int hal_nvm_read(uint16_t key, void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* LOCK_HAL_H */
