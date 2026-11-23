/*
 * Board bring-up program: exercises every block of a freshly assembled
 * mainboard through the same HAL the lock firmware uses, and prints the
 * results on VCOM (PA8/PA9, 115200 8N1). Run it before anything else.
 *
 *   1. LEDs red / green / blue, buzzer
 *   2. battery voltage (divider + ADC)
 *   3. motor: engage, release, with current and stall detection
 *   4. every switch input: live state, and an event line on each change
 *   5. power rails NFC / fingerprint / face / backlight toggled one by one
 *      (measure with the current meter)
 *   6. idle: the power manager keeps the chip in EM2 between events, so the
 *      sleep current can be measured; input events still print
 *
 * Simplicity SDK: create an "Empty C Project" for EFR32FG28B322F1024IM68,
 * add the components listed in hal_efr32.c plus iostream_eusart (EUSART0 on
 * PA8/PA9) and power_manager, and add hal_efr32.c, crypto_psa.c and this file.
 */
#include <stdio.h>

#include "board.h"
#include "hal.h"
#include "hal_efr32.h"
#include "sl_sleeptimer.h"

static volatile uint32_t events;

static void on_input(hal_efr32_input_t in) { events |= 1u << in; }

static const char *names[] = { "keypad", "nfc", "finger", "presence", "door",
                               "double-lock", "tamper", "mech-key", "setup-key" };

void app_init(void)
{
    hal_efr32_init(on_input);
    printf("\n%s bring-up, EUI %016llx\n", BOARD_NAME, (unsigned long long)hal_efr32_eui());

    printf("[1] LED / buzzer\n");
    hal_indicate(HAL_IND_OK);
    hal_indicate(HAL_IND_DENIED);
    hal_efr32_led_unbound(true);
    sl_sleeptimer_delay_millisecond(500);
    hal_efr32_led_unbound(false);

    printf("[2] battery %u mV\n", hal_battery_mv());

    printf("[3] motor engage -> %s\n", hal_clutch_drive(true) == 0 ? "ok" : "STALL/TIMEOUT");
    sl_sleeptimer_delay_millisecond(500);
    printf("    motor release -> %s\n", hal_clutch_drive(false) == 0 ? "ok" : "STALL/TIMEOUT");

    printf("[4] inputs:");
    for (int i = 0; i <= HAL_IN_SETUP_KEY; i++) {
        printf(" %s=%d", names[i], hal_efr32_input_active((hal_efr32_input_t)i));
    }
    printf("\n");

    printf("[5] power rails, 2 s each\n");
    for (int r = 0; r <= HAL_RAIL_BACKLIGHT; r++) {
        hal_efr32_power((hal_efr32_rail_t)r, true);
        sl_sleeptimer_delay_millisecond(2000);
        hal_efr32_power((hal_efr32_rail_t)r, false);
    }

    printf("[6] idle in EM2: measure the sleep current now; touch inputs to see events\n");
}

void app_process_action(void)
{
    uint32_t e = events;
    if (e) {
        events = 0;
        for (int i = 0; i <= HAL_IN_SETUP_KEY; i++) {
            if (e & (1u << i)) {
                printf("event %s -> %d\n", names[i], hal_efr32_input_active((hal_efr32_input_t)i));
            }
        }
    }
}
