/*
 * EFR32 board extras beyond hal.h: input events, power rails, EUI, key
 * handles. Used by the EFR32 main loop and the bring-up program.
 */
#ifndef LOCK_HAL_EFR32_H
#define LOCK_HAL_EFR32_H

#include <stdbool.h>
#include <stdint.h>

#include "slp/crypto_port.h"

typedef enum {
    HAL_IN_KEYPAD,      /* touch keypad touched (wake, BLE advertising window) */
    HAL_IN_NFC,         /* reader found a card in low-power card detection */
    HAL_IN_FINGER,      /* finger on the sensor */
    HAL_IN_PRESENCE,    /* someone in front of the door (face version) */
    HAL_IN_DOOR,        /* door sensor changed */
    HAL_IN_DOUBLE_LOCK, /* indoor double-lock knob changed */
    HAL_IN_TAMPER,      /* panel removed */
    HAL_IN_MECH_KEY,    /* mechanical override key turned */
    HAL_IN_SETUP_KEY    /* indoor setup / reset key */
} hal_efr32_input_t;

typedef enum {
    HAL_RAIL_NFC,
    HAL_RAIL_FINGERPRINT,
    HAL_RAIL_FACE,      /* 5 V boost + module */
    HAL_RAIL_BACKLIGHT
} hal_efr32_rail_t;

/* Called from interrupt context: set a flag or post an event only. */
typedef void (*hal_efr32_input_cb_t)(hal_efr32_input_t in);

void hal_efr32_init(hal_efr32_input_cb_t cb);
bool hal_efr32_input_active(hal_efr32_input_t in);
void hal_efr32_power(hal_efr32_rail_t rail, bool on);
void hal_efr32_led_unbound(bool on);
uint64_t hal_efr32_eui(void);

/* PSA key id of the active K_app (slot from the binding state). */
slp_key_handle_t hal_efr32_app_key(void);

#endif
