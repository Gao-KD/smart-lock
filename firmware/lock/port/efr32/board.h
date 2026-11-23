/*
 * Board definition of the lock mainboard, revision A (EFR32FG28B322F1024IM68).
 *
 * THIS FILE IS THE PIN MAP. The schematic must follow it, and the firmware
 * reads every pin from here, so a board drawn to docs/hardware/board-spec.md
 * runs the firmware unchanged. If the hardware has to move a signal, change
 * it here (and in the spec) in the same commit; tests/test_board.c checks
 * the constraints below on every build.
 *
 * Facts checked against the Silicon Labs device header
 * efr32fg28b322f1024im68.h (Simplicity SDK 2024.12):
 *   - GPIO: PA0..PA14 (15), PB0..PB5 (6), PC0..PC11 (12), PD0..PD15 (16) = 49
 *   - SWCLK PA1, SWDIO PA2, SWV PA3 (keep free for debug)
 *   - LFXO: LFXTAL_O PD0, LFXTAL_I PD1 (32.768 kHz crystal)
 *   - EM4 wake-up pins: PA5, PB1, PB3, PC0, PC5, PC7, PD2, PD5
 *   - Datasheet: only ports A and B work in EM2/EM3 (wake-up interrupts,
 *     low-energy peripherals); ports C and D are latched in EM2.
 *
 * Rules used for the assignment:
 *   - every input that must wake the lock from deep sleep is on port A/B;
 *   - the setup key and the tamper switch are on EM4 wake-up pins;
 *   - outputs that must hold a level while asleep (load switches off, motor
 *     driver asleep) may sit on C/D: their state is latched in EM2;
 *   - power enables default OFF through a pull-down on the board.
 *
 * Plain integers only (no SDK types), so the host build can check the map.
 */
#ifndef LOCK_BOARD_H
#define LOCK_BOARD_H

#define BOARD_NAME "smart-lock mainboard rev A"

/* Port indexes, same values as GPIO_PA_INDEX..GPIO_PD_INDEX. */
#define BOARD_PORT_A 0
#define BOARD_PORT_B 1
#define BOARD_PORT_C 2
#define BOARD_PORT_D 3

/* ---------------- Port A: EM2-capable inputs, keypad I2C, debug ---------------- */
#define BOARD_KEY_INT_PORT     BOARD_PORT_A  /* touch keypad IRQ, active low, wakes EM2 */
#define BOARD_KEY_INT_PIN      0
/*                             PA1 SWCLK, PA2 SWDIO, PA3 SWV: debug / production test */
#define BOARD_NFC_IRQ_PORT     BOARD_PORT_A  /* FM17550 IRQ (card detected in LPCD), wakes EM2 */
#define BOARD_NFC_IRQ_PIN      4
#define BOARD_SETUP_KEY_PORT   BOARD_PORT_A  /* indoor setup / reset key, active low, EM4WU0 */
#define BOARD_SETUP_KEY_PIN    5
#define BOARD_FP_TOUCH_PORT    BOARD_PORT_A  /* fingerprint finger-detect, wakes EM2 */
#define BOARD_FP_TOUCH_PIN     6
#define BOARD_DOOR_HALL_PORT   BOARD_PORT_A  /* DRV5032 door sensor, low = magnet near (door closed) */
#define BOARD_DOOR_HALL_PIN    7
#define BOARD_VCOM_TX_PORT     BOARD_PORT_A  /* EUSART0 TX: debug log / production test */
#define BOARD_VCOM_TX_PIN      8
#define BOARD_VCOM_RX_PORT     BOARD_PORT_A  /* EUSART0 RX */
#define BOARD_VCOM_RX_PIN      9
#define BOARD_PROX_INT_PORT    BOARD_PORT_A  /* face version: radar / PIR presence, wakes EM2 */
#define BOARD_PROX_INT_PIN     10
#define BOARD_MECH_KEY_PORT    BOARD_PORT_A  /* mechanical override key micro switch, active low */
#define BOARD_MECH_KEY_PIN     11
#define BOARD_KEY_SCL_PORT     BOARD_PORT_A  /* I2C0 SCL to the touch keypad controller */
#define BOARD_KEY_SCL_PIN      12
#define BOARD_KEY_SDA_PORT     BOARD_PORT_A  /* I2C0 SDA */
#define BOARD_KEY_SDA_PIN      13
#define BOARD_BUZZER_PORT      BOARD_PORT_A  /* passive buzzer via NPN, TIMER PWM */
#define BOARD_BUZZER_PIN       14

/* ---------------- Port B: more wake-up inputs, analog ---------------- */
#define BOARD_DBL_LOCK_PORT    BOARD_PORT_B  /* indoor double-lock knob switch, active low */
#define BOARD_DBL_LOCK_PIN     0
#define BOARD_TAMPER_PORT      BOARD_PORT_B  /* panel tamper switch, active low, EM4WU3 */
#define BOARD_TAMPER_PIN       1
#define BOARD_VBAT_ADC_PORT    BOARD_PORT_B  /* battery divider output -> IADC0 */
#define BOARD_VBAT_ADC_PIN     2
#define BOARD_SPARE_WAKE_PORT  BOARD_PORT_B  /* spare EM4WU4: route to the FPC spare line */
#define BOARD_SPARE_WAKE_PIN   3
#define BOARD_MOT_ISENSE_PORT  BOARD_PORT_B  /* motor current sense resistor -> IADC0 */
#define BOARD_MOT_ISENSE_PIN   4
#define BOARD_LED_R_PORT       BOARD_PORT_B  /* status LED red, active high */
#define BOARD_LED_R_PIN        5

/* ---------------- Port C: NFC SPI, motor, enables ---------------- */
#define BOARD_NFC_SCK_PORT     BOARD_PORT_C  /* USART0 SPI CLK */
#define BOARD_NFC_SCK_PIN      0
#define BOARD_NFC_MOSI_PORT    BOARD_PORT_C  /* USART0 TX */
#define BOARD_NFC_MOSI_PIN     1
#define BOARD_NFC_MISO_PORT    BOARD_PORT_C  /* USART0 RX */
#define BOARD_NFC_MISO_PIN     2
#define BOARD_NFC_CS_PORT      BOARD_PORT_C  /* chip select, active low */
#define BOARD_NFC_CS_PIN       3
#define BOARD_NFC_NRST_PORT    BOARD_PORT_C  /* FM17550 NRSTPD: low = hard power down */
#define BOARD_NFC_NRST_PIN     4
#define BOARD_NFC_PWR_EN_PORT  BOARD_PORT_C  /* load switch for the reader, active high */
#define BOARD_NFC_PWR_EN_PIN   5
#define BOARD_MOT_IN1_PORT     BOARD_PORT_C  /* DRV8837 IN1 */
#define BOARD_MOT_IN1_PIN      6
#define BOARD_MOT_IN2_PORT     BOARD_PORT_C  /* DRV8837 IN2 */
#define BOARD_MOT_IN2_PIN      7
#define BOARD_MOT_NSLEEP_PORT  BOARD_PORT_C  /* DRV8837 nSLEEP: low = driver asleep */
#define BOARD_MOT_NSLEEP_PIN   8
#define BOARD_VBAT_EN_PORT     BOARD_PORT_C  /* connects the battery divider only while sampling */
#define BOARD_VBAT_EN_PIN      9
#define BOARD_LED_G_PORT       BOARD_PORT_C  /* status LED green, active high */
#define BOARD_LED_G_PIN        10
#define BOARD_LED_B_PORT       BOARD_PORT_C  /* status LED blue (unbound / pairing), active high */
#define BOARD_LED_B_PIN        11

/* ---------------- Port D: LFXO, fingerprint and face UARTs ---------------- */
/*                             PD0 LFXTAL_O, PD1 LFXTAL_I: 32.768 kHz crystal */
#define BOARD_FP_TX_PORT       BOARD_PORT_D  /* EUSART1 TX -> fingerprint module RX */
#define BOARD_FP_TX_PIN        2
#define BOARD_FP_RX_PORT       BOARD_PORT_D  /* EUSART1 RX <- fingerprint module TX */
#define BOARD_FP_RX_PIN        3
#define BOARD_FP_PWR_EN_PORT   BOARD_PORT_D  /* fingerprint load switch, active high */
#define BOARD_FP_PWR_EN_PIN    4
#define BOARD_FACE_PWR_EN_PORT BOARD_PORT_D  /* 5 V regulator enable (face module), active high */
#define BOARD_FACE_PWR_EN_PIN  5
#define BOARD_FACE_TX_PORT     BOARD_PORT_D  /* EUSART2 TX -> face module RX */
#define BOARD_FACE_TX_PIN      6
#define BOARD_FACE_RX_PORT     BOARD_PORT_D  /* EUSART2 RX <- face module TX */
#define BOARD_FACE_RX_PIN      7
#define BOARD_KEY_BL_EN_PORT   BOARD_PORT_D  /* keypad backlight LEDs, active high */
#define BOARD_KEY_BL_EN_PIN    8
/*                             PD9..PD15: unused, bring to test pads */

/* ---------------- GPIO external interrupt numbers ----------------
 * Series 2 rule: interrupt n can only take a pin from the same group of
 * four (n / 4 == pin / 4). Checked by tests/test_board.c. */
#define BOARD_KEY_INT_EXTI    0
#define BOARD_DBL_LOCK_EXTI   1
#define BOARD_TAMPER_EXTI     2
#define BOARD_NFC_IRQ_EXTI    4
#define BOARD_SETUP_KEY_EXTI  5
#define BOARD_FP_TOUCH_EXTI   6
#define BOARD_DOOR_HALL_EXTI  7
#define BOARD_PROX_INT_EXTI   10
#define BOARD_MECH_KEY_EXTI   11

/* ---------------- Peripheral instances ---------------- */
#define BOARD_NFC_USART      0     /* USART0, SPI mode 0, 4 MHz */
#define BOARD_NFC_SPI_HZ     4000000
#define BOARD_KEY_I2C        0     /* I2C0, 100 kHz */
#define BOARD_VCOM_EUSART    0     /* EUSART0, 115200 8N1 */
#define BOARD_FP_EUSART      1     /* EUSART1, baud per module (typically 57600) */
#define BOARD_FACE_EUSART    2     /* EUSART2, baud per module (typically 115200) */

/* ---------------- Analog front ends ---------------- */
/* Battery: VBAT -- 1 MOhm -- ADC -- 330 kOhm -- GND, switched by VBAT_EN.
 * VBAT (mV) = ADC (mV) * (1000 + 330) / 330. Keeps 8.4 V (2S Li-ion) below
 * the 1.21 V internal reference x 2 (IADC gain 0.5). */
#define BOARD_VBAT_DIV_NUM   1330
#define BOARD_VBAT_DIV_DEN   330
/* Motor current: sense resistor in the DRV8837 low side. */
#define BOARD_MOT_RSENSE_MOHM 220
/* Stall: current above this for BOARD_MOT_STALL_MS means the clutch is
 * blocked; normal travel of the reference clutch motor is ~120-180 mA. */
#define BOARD_MOT_STALL_MA    450
#define BOARD_MOT_STALL_MS    40
/* Clutch travel time (engage or release), and max drive time. */
#define BOARD_MOT_DRIVE_MS    150
#define BOARD_MOT_TIMEOUT_MS  400

/* PSA persistent key ids (Secure Vault). Slot A/B for the two-phase key
 * commit, see docs/provisioning.md section 4. */
#define BOARD_KEY_ID_DEV       0x00010000u
#define BOARD_KEY_ID_SLOT(s)   (0x00010100u + 0x10u * (uint32_t)(s))
#define BOARD_KEY_ID_LINK(s)   (BOARD_KEY_ID_SLOT(s) + 1u)
#define BOARD_KEY_ID_APP(s)    (BOARD_KEY_ID_SLOT(s) + 2u)
#define BOARD_KEY_ID_GRP(s)    (BOARD_KEY_ID_SLOT(s) + 3u)

/* Switch inputs: all active low with the internal pull-up and the glitch
 * filter enabled; the board adds 100 nF to GND on each (debounce, ESD). */
#define BOARD_INPUT_ACTIVE_LOW 1

#endif /* LOCK_BOARD_H */
