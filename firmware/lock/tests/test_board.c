/*
 * Checks the board pin map (port/efr32/board.h) against the EFR32FG28 QFN68
 * facts and the wake-up rules, so a pin change that breaks the hardware
 * contract fails the build instead of a prototype.
 */
#include <stdio.h>
#include <string.h>

#include "unit.h"
#include "board.h"

typedef struct {
    const char *name;
    int port, pin;
    int em2_wake; /* must wake the chip from EM2: port A/B */
    int em4_wake; /* must sit on an EM4WU pin */
} sig_t;

#define S(n, w2, w4) { #n, BOARD_##n##_PORT, BOARD_##n##_PIN, w2, w4 }
static const sig_t sigs[] = {
    S(KEY_INT, 1, 0),    S(NFC_IRQ, 1, 0),   S(SETUP_KEY, 1, 1), S(FP_TOUCH, 1, 0),
    S(DOOR_HALL, 1, 0),  S(VCOM_TX, 0, 0),   S(VCOM_RX, 0, 0),   S(PROX_INT, 1, 0),
    S(MECH_KEY, 1, 0),   S(KEY_SCL, 0, 0),   S(KEY_SDA, 0, 0),   S(BUZZER, 0, 0),
    S(DBL_LOCK, 1, 0),   S(TAMPER, 1, 1),    S(VBAT_ADC, 0, 0),  S(SPARE_WAKE, 1, 1),
    S(MOT_ISENSE, 0, 0), S(LED_R, 0, 0),     S(NFC_SCK, 0, 0),   S(NFC_MOSI, 0, 0),
    S(NFC_MISO, 0, 0),   S(NFC_CS, 0, 0),    S(NFC_NRST, 0, 0),  S(NFC_PWR_EN, 0, 0),
    S(MOT_IN1, 0, 0),    S(MOT_IN2, 0, 0),   S(MOT_NSLEEP, 0, 0), S(VBAT_EN, 0, 0),
    S(LED_G, 0, 0),      S(LED_B, 0, 0),     S(FP_TX, 0, 0),     S(FP_RX, 0, 0),
    S(FP_PWR_EN, 0, 0),  S(FACE_PWR_EN, 0, 0), S(FACE_TX, 0, 0), S(FACE_RX, 0, 0),
    S(KEY_BL_EN, 0, 0),
};
#define NSIG ((int)(sizeof(sigs) / sizeof(sigs[0])))

/* EFR32FG28B322F1024IM68: pins per port (device header GPIO_Px_COUNT). */
static const int port_pins[4] = { 15, 6, 12, 16 };
/* Reserved: SWCLK PA1, SWDIO PA2, SWV PA3, LFXTAL_O PD0, LFXTAL_I PD1. */
static const int reserved[][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 3, 0 }, { 3, 1 } };
/* EM4 wake-up pins (GPIO_EM4WUx_PORT/PIN). */
static const int em4wu[][2] = { { 0, 5 }, { 1, 1 }, { 1, 3 }, { 2, 0 }, { 2, 5 }, { 2, 7 }, { 3, 2 }, { 3, 5 } };

static int in_list(const int (*l)[2], int n, int port, int pin)
{
    for (int i = 0; i < n; i++) {
        if (l[i][0] == port && l[i][1] == pin) return 1;
    }
    return 0;
}

static void test_pins_exist_and_unique(void)
{
    for (int i = 0; i < NSIG; i++) {
        const sig_t *s = &sigs[i];
        CHECK(s->port >= 0 && s->port < 4);
        CHECK(s->pin >= 0 && s->pin < port_pins[s->port]);
        if (in_list(reserved, 5, s->port, s->pin)) {
            printf("  %s uses a reserved pin (debug / LFXO)\n", s->name);
            CHECK(0);
        }
        for (int j = i + 1; j < NSIG; j++) {
            if (sigs[j].port == s->port && sigs[j].pin == s->pin) {
                printf("  %s and %s share P%c%d\n", s->name, sigs[j].name, 'A' + s->port, s->pin);
                CHECK(0);
            }
        }
    }
}

static void test_wake_rules(void)
{
    for (int i = 0; i < NSIG; i++) {
        const sig_t *s = &sigs[i];
        if (s->em2_wake && s->port > BOARD_PORT_B) {
            printf("  %s must be on port A/B to wake from EM2\n", s->name);
            CHECK(0);
        }
        if (s->em4_wake && !in_list(em4wu, 8, s->port, s->pin)) {
            printf("  %s must be on an EM4WU pin\n", s->name);
            CHECK(0);
        }
    }
}

static void test_exti_groups(void)
{
    /* Series 2: interrupt n takes a pin from its own group of four. */
    const struct { const char *name; int pin, exti; } e[] = {
        { "KEY_INT", BOARD_KEY_INT_PIN, BOARD_KEY_INT_EXTI },
        { "DBL_LOCK", BOARD_DBL_LOCK_PIN, BOARD_DBL_LOCK_EXTI },
        { "TAMPER", BOARD_TAMPER_PIN, BOARD_TAMPER_EXTI },
        { "NFC_IRQ", BOARD_NFC_IRQ_PIN, BOARD_NFC_IRQ_EXTI },
        { "SETUP_KEY", BOARD_SETUP_KEY_PIN, BOARD_SETUP_KEY_EXTI },
        { "FP_TOUCH", BOARD_FP_TOUCH_PIN, BOARD_FP_TOUCH_EXTI },
        { "DOOR_HALL", BOARD_DOOR_HALL_PIN, BOARD_DOOR_HALL_EXTI },
        { "PROX_INT", BOARD_PROX_INT_PIN, BOARD_PROX_INT_EXTI },
        { "MECH_KEY", BOARD_MECH_KEY_PIN, BOARD_MECH_KEY_EXTI },
    };
    int n = (int)(sizeof(e) / sizeof(e[0]));
    for (int i = 0; i < n; i++) {
        CHECK(e[i].exti >= 0 && e[i].exti < 16);
        if (e[i].exti / 4 != e[i].pin / 4) {
            printf("  %s: EXTI %d cannot take pin %d\n", e[i].name, e[i].exti, e[i].pin);
            CHECK(0);
        }
        for (int j = i + 1; j < n; j++) {
            CHECK(e[i].exti != e[j].exti);
        }
    }
}

static void test_divider_range(void)
{
    /* 2S Li-ion full (8.4 V) must stay below the IADC full scale (2.42 V). */
    CHECK(8400L * BOARD_VBAT_DIV_DEN / BOARD_VBAT_DIV_NUM < 2420L);
    /* 4xAA empty (4.0 V) still well above the ADC noise floor. */
    CHECK(4000L * BOARD_VBAT_DIV_DEN / BOARD_VBAT_DIV_NUM > 500L);
}

int main(void)
{
    printf("board pin map tests (%s, %d signals)\n", BOARD_NAME, NSIG);
    RUN(test_pins_exist_and_unique);
    RUN(test_wake_rules);
    RUN(test_exti_groups);
    RUN(test_divider_range);
    UNIT_DONE();
}
