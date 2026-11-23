/*
 * hal.h for the EFR32FG28 lock mainboard (pins: board.h).
 *
 * Simplicity SDK components this file needs (add them in the .slcp):
 *   emlib_gpio, emlib_iadc, emlib_cmu, sleeptimer, udelay, nvm3_default,
 *   psa_crypto (+ psa_crypto_ccm), gpiointerrupt, device_init (LFXO, HFXO 39 MHz)
 *
 * Status: written against the SDK APIs, NOT yet compiled on a board. The
 * first bring-up compiles it in Simplicity Studio together with
 * bringup_main.c (see README.md). Everything above this layer (lock logic,
 * credentials, protocol, MAC) is the same code that runs in the host tests
 * and the simulator.
 */
#include "hal.h"

#include <string.h>

#include "board.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_iadc.h"
#include "em_system.h"
#include "gpiointerrupt.h"
#include "nvm3_default.h"
#include "sl_sleeptimer.h"
#include "sl_udelay.h"
#include "slp/secure.h"

#include "hal_efr32.h"

#define PORT(x) ((GPIO_Port_TypeDef)BOARD_##x##_PORT)
#define PIN(x)  ((unsigned int)BOARD_##x##_PIN)
#define OUT_LOW(x)  GPIO_PinModeSet(PORT(x), PIN(x), gpioModePushPull, 0)
#define SET(x, v)   do { if (v) GPIO_PinOutSet(PORT(x), PIN(x)); else GPIO_PinOutClear(PORT(x), PIN(x)); } while (0)
#define IN_PULLUP(x) GPIO_PinModeSet(PORT(x), PIN(x), gpioModeInputPullFilter, 1)
#define READ_LOW(x)  (GPIO_PinInGet(PORT(x), PIN(x)) == 0)

static int32_t  unix_offset;  /* unix time = offset + seconds since boot */
static uint64_t lock_eui;
static hal_efr32_input_cb_t input_cb;

/* ---------------- time ---------------- */

static uint64_t ms64(void)
{
    uint64_t ms = 0;
    (void)sl_sleeptimer_tick64_to_ms(sl_sleeptimer_get_tick_count64(), &ms);
    return ms;
}

uint32_t hal_millis(void) { return (uint32_t)ms64(); }

uint32_t hal_unix_time(void) { return (uint32_t)((int64_t)unix_offset + (int64_t)(ms64() / 1000u)); }

void hal_set_unix_time(uint32_t unix_s)
{
    unix_offset = (int32_t)((int64_t)unix_s - (int64_t)(ms64() / 1000u));
}

/* ---------------- ADC (battery, motor current) ---------------- */

static void adc_init(void)
{
    CMU_ClockEnable(cmuClock_IADC0, true);
    CMU_ClockSelectSet(cmuClock_IADCCLK, cmuSelect_FSRCO);
    IADC_Init_t init = IADC_INIT_DEFAULT;
    IADC_AllConfigs_t cfg = IADC_ALLCONFIGS_DEFAULT;
    init.srcClkPrescale = IADC_calcSrcClkPrescale(IADC0, 10000000, 0);
    cfg.configs[0].reference = iadcCfgReferenceInt1V2;
    cfg.configs[0].vRef = 1210;
    cfg.configs[0].analogGain = iadcCfgAnalogGain0P5x; /* full scale 2.42 V */
    cfg.configs[0].adcClkPrescale =
        IADC_calcAdcClkPrescale(IADC0, 1000000, 0, iadcCfgModeNormal, init.srcClkPrescale);
    IADC_init(IADC0, &init, &cfg);
    /* PB2 (VBAT) and PB4 (motor current) are even pins of port B. */
    GPIO->BBUSALLOC |= GPIO_BBUSALLOC_BEVEN0_ADC0;
}

/* One conversion on a port-B pin, in millivolts at the pin. */
static uint32_t adc_read_mv(unsigned int pin)
{
    IADC_InitSingle_t is = IADC_INITSINGLE_DEFAULT;
    IADC_SingleInput_t in = IADC_SINGLEINPUT_DEFAULT;
    in.posInput = IADC_portPinToPosInput(gpioPortB, pin);
    in.negInput = iadcNegInputGnd;
    IADC_initSingle(IADC0, &is, &in);
    IADC_command(IADC0, iadcCmdStartSingle);
    while ((IADC0->STATUS & IADC_STATUS_SINGLEFIFODV) == 0) {
    }
    IADC_Result_t r = IADC_pullSingleFifoResult(IADC0);
    return (r.data * 2420u) / 4095u;
}

uint16_t hal_battery_mv(void)
{
    SET(VBAT_EN, 1);
    sl_udelay_wait(200); /* divider + 100 nF settle */
    uint32_t mv = adc_read_mv(PIN(VBAT_ADC));
    SET(VBAT_EN, 0);
    return (uint16_t)(mv * BOARD_VBAT_DIV_NUM / BOARD_VBAT_DIV_DEN);
}

static uint32_t motor_ma(void)
{
    uint32_t mv = adc_read_mv(PIN(MOT_ISENSE));
    return mv * 1000u / BOARD_MOT_RSENSE_MOHM;
}

/* ---------------- clutch motor (DRV8837) ---------------- */

int hal_clutch_drive(bool engage)
{
    SET(MOT_NSLEEP, 1);
    sl_udelay_wait(50); /* DRV8837 wake-up time */
    SET(MOT_IN1, engage);
    SET(MOT_IN2, !engage);
    uint32_t start = hal_millis(), over_since = 0;
    int rc = 0;
    while (hal_millis() - start < BOARD_MOT_DRIVE_MS) {
        uint32_t ma = motor_ma();
        if (ma > BOARD_MOT_STALL_MA) {
            if (over_since == 0) {
                over_since = hal_millis() ? hal_millis() : 1;
            } else if (hal_millis() - over_since >= BOARD_MOT_STALL_MS) {
                rc = -1; /* stalled: clutch blocked */
                break;
            }
        } else {
            over_since = 0;
        }
        if (hal_millis() - start > BOARD_MOT_TIMEOUT_MS) {
            rc = -1;
            break;
        }
    }
    /* brake briefly, then coast and sleep the driver */
    SET(MOT_IN1, 1);
    SET(MOT_IN2, 1);
    sl_udelay_wait(5000);
    SET(MOT_IN1, 0);
    SET(MOT_IN2, 0);
    SET(MOT_NSLEEP, 0);
    return rc;
}

/* ---------------- LEDs and buzzer ---------------- */

static void beep(uint32_t ms, uint32_t hz)
{
    uint32_t half_us = 500000u / hz;
    for (uint32_t t = 0; t < ms * 1000u; t += 2u * half_us) {
        SET(BUZZER, 1);
        sl_udelay_wait(half_us);
        SET(BUZZER, 0);
        sl_udelay_wait(half_us);
    }
}

static void leds(bool r, bool g, bool b)
{
    SET(LED_R, r);
    SET(LED_G, g);
    SET(LED_B, b);
}

void hal_indicate(hal_indication_t ind)
{
    switch (ind) {
    case HAL_IND_OK:
        leds(false, true, false);
        beep(80, 2700);
        sl_sleeptimer_delay_millisecond(300);
        break;
    case HAL_IND_DENIED:
        leds(true, false, false);
        beep(400, 2000);
        break;
    case HAL_IND_LOW_BATT:
        leds(true, false, false);
        for (int i = 0; i < 3; i++) {
            beep(60, 2700);
            sl_sleeptimer_delay_millisecond(80);
        }
        break;
    case HAL_IND_ALARM:
        leds(true, false, true);
        for (int i = 0; i < 10; i++) {
            beep(150, 3200);
            beep(150, 2400);
        }
        break;
    }
    leds(false, false, false);
}

void hal_efr32_led_unbound(bool on) { SET(LED_B, on); }

/* ---------------- keys and storage ---------------- */

int hal_app_mac(const uint8_t *msg, size_t len, uint8_t out[8])
{
    return slp_app_mac(hal_efr32_app_key(), lock_eui, msg, len, out);
}

int hal_nvm_write(uint16_t key, const void *data, size_t len)
{
    return nvm3_writeData(nvm3_defaultHandle, (nvm3_ObjectKey_t)key, data, len) == ECODE_NVM3_OK ? 0 : -1;
}

int hal_nvm_read(uint16_t key, void *data, size_t len)
{
    return nvm3_readData(nvm3_defaultHandle, (nvm3_ObjectKey_t)key, data, len) == ECODE_NVM3_OK ? 0 : -1;
}

uint64_t hal_efr32_eui(void) { return lock_eui; }

/* ---------------- inputs ---------------- */

static void on_exti(uint8_t intno)
{
    hal_efr32_input_t in;
    switch (intno) {
    case BOARD_KEY_INT_EXTI:   in = HAL_IN_KEYPAD; break;
    case BOARD_DBL_LOCK_EXTI:  in = HAL_IN_DOUBLE_LOCK; break;
    case BOARD_TAMPER_EXTI:    in = HAL_IN_TAMPER; break;
    case BOARD_NFC_IRQ_EXTI:   in = HAL_IN_NFC; break;
    case BOARD_SETUP_KEY_EXTI: in = HAL_IN_SETUP_KEY; break;
    case BOARD_FP_TOUCH_EXTI:  in = HAL_IN_FINGER; break;
    case BOARD_DOOR_HALL_EXTI: in = HAL_IN_DOOR; break;
    case BOARD_PROX_INT_EXTI:  in = HAL_IN_PRESENCE; break;
    case BOARD_MECH_KEY_EXTI:  in = HAL_IN_MECH_KEY; break;
    default: return;
    }
    if (input_cb) {
        input_cb(in); /* ISR context: only set a flag / post an event */
    }
}

#define WAKE_INPUT(x, both)                                                                 \
    do {                                                                                    \
        IN_PULLUP(x);                                                                       \
        GPIOINT_CallbackRegister(BOARD_##x##_EXTI, on_exti);                                \
        GPIO_ExtIntConfig(PORT(x), PIN(x), BOARD_##x##_EXTI, (both), true, true);           \
    } while (0)

bool hal_efr32_input_active(hal_efr32_input_t in)
{
    switch (in) {
    case HAL_IN_DOOR:        return !READ_LOW(DOOR_HALL); /* high = magnet away = door open */
    case HAL_IN_DOUBLE_LOCK: return READ_LOW(DBL_LOCK);
    case HAL_IN_TAMPER:      return READ_LOW(TAMPER);
    case HAL_IN_MECH_KEY:    return READ_LOW(MECH_KEY);
    case HAL_IN_SETUP_KEY:   return READ_LOW(SETUP_KEY);
    case HAL_IN_KEYPAD:      return READ_LOW(KEY_INT);
    case HAL_IN_FINGER:      return READ_LOW(FP_TOUCH);
    case HAL_IN_NFC:         return READ_LOW(NFC_IRQ);
    case HAL_IN_PRESENCE:    return !READ_LOW(PROX_INT);
    }
    return false;
}

void hal_efr32_power(hal_efr32_rail_t rail, bool on)
{
    switch (rail) {
    case HAL_RAIL_NFC:
        SET(NFC_PWR_EN, on);
        SET(NFC_NRST, on);
        break;
    case HAL_RAIL_FINGERPRINT: SET(FP_PWR_EN, on); break;
    case HAL_RAIL_FACE:        SET(FACE_PWR_EN, on); break;
    case HAL_RAIL_BACKLIGHT:   SET(KEY_BL_EN, on); break;
    }
}

void hal_efr32_init(hal_efr32_input_cb_t cb)
{
    input_cb = cb;
    CMU_ClockEnable(cmuClock_GPIO, true);
    lock_eui = SYSTEM_GetUnique();

    /* outputs: everything off */
    OUT_LOW(NFC_PWR_EN);
    OUT_LOW(NFC_NRST);
    GPIO_PinModeSet(PORT(NFC_CS), PIN(NFC_CS), gpioModePushPull, 1);
    OUT_LOW(FP_PWR_EN);
    OUT_LOW(FACE_PWR_EN);
    OUT_LOW(KEY_BL_EN);
    OUT_LOW(MOT_IN1);
    OUT_LOW(MOT_IN2);
    OUT_LOW(MOT_NSLEEP);
    OUT_LOW(VBAT_EN);
    OUT_LOW(BUZZER);
    OUT_LOW(LED_R);
    OUT_LOW(LED_G);
    OUT_LOW(LED_B);
    GPIO_PinModeSet(PORT(VBAT_ADC), PIN(VBAT_ADC), gpioModeDisabled, 0);
    GPIO_PinModeSet(PORT(MOT_ISENSE), PIN(MOT_ISENSE), gpioModeDisabled, 0);

    /* wake-up inputs (port A/B, EM2) */
    GPIOINT_Init();
    WAKE_INPUT(KEY_INT, false);
    WAKE_INPUT(NFC_IRQ, false);
    WAKE_INPUT(SETUP_KEY, false);
    WAKE_INPUT(FP_TOUCH, false);
    WAKE_INPUT(DOOR_HALL, true);
    WAKE_INPUT(PROX_INT, true);
    WAKE_INPUT(MECH_KEY, false);
    WAKE_INPUT(DBL_LOCK, true);
    WAKE_INPUT(TAMPER, false);
    /* setup key and tamper also wake from EM4 (shipping / deep sleep) */
    GPIO_EM4EnablePinWakeup(GPIO_IEN_EM4WUIEN0 << 0, 0);   /* EM4WU0 = PA5, active low */
    GPIO_EM4EnablePinWakeup(GPIO_IEN_EM4WUIEN0 << 3, 0);   /* EM4WU3 = PB1, active low */

    adc_init();
}
