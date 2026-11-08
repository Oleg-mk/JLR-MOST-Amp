#pragma once
/* ESP32-WROVER-IE pins as wired on the main board (hw/gen/build.py, host sheet) */
#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"

#define PIN_BCK        GPIO_NUM_15   /* in: BCK 64Fs = 3.072 MHz from OS8104A, both I2S */
#define PIN_LRCK       GPIO_NUM_4    /* in: LRCK = FSY 48 kHz from OS8104A, both I2S */
#define PIN_MOST_SX1   GPIO_NUM_14   /* in: I2S0 data from SX1 */
#define PIN_SPDIF_SRC  GPIO_NUM_12   /* out: I2S0 data to SR1, OS8104A makes S/PDIF of it; strapping pin, R17 holds it low */
#define PIN_MOST_SX3   GPIO_NUM_5    /* in: I2S1 data from SX3 */
#define PIN_DOUT       GPIO_NUM_2    /* out: I2S1 data to PCM5122 and the HDMI LVDS driver */
#define PIN_MOST_SDA   GPIO_NUM_26   /* I2C0: OS8104A */
#define PIN_MOST_SCL   GPIO_NUM_27
#define PIN_MOST_INT   GPIO_NUM_25   /* in: OS8104A interrupt, active low, R10 pull-up */
#define PIN_MOST_RST   GPIO_NUM_13   /* open drain: OS8104A reset, R12 + C14 make the power-on reset */
#define PIN_DAC_SDA    GPIO_NUM_21   /* I2C1: PCM5122 */
#define PIN_DAC_SCL    GPIO_NUM_19
#define PIN_HOLD       GPIO_NUM_33   /* out: keeps the supply on after the light in the ring is gone */
#define PIN_REM_EN     GPIO_NUM_32   /* out: REM +12 V switch */
#define PIN_LED        GPIO_NUM_22   /* out: D20 */
#define PIN_MODE0      GPIO_NUM_36   /* in: SW1 key 4, "read serial number"; a closed key reads 0 */
#define PIN_MODE1      GPIO_NUM_39   /* in: SW1 keys 3, 2, 1 = MODE1, MODE2, MODE3: amplifier type */
#define PIN_MODE2      GPIO_NUM_34
#define PIN_MODE3      GPIO_NUM_35
#define PIN_TXD0       GPIO_NUM_1    /* out: UART0 TX to the CH340C (log and console) */

#define I2C_ADDR_OS8104A  0x23       /* 0x46 / 0x47 on the wire, AD0 = AD1 = 1 */
#define I2C_ADDR_PCM5122  0x4C

void board_init(void);
void board_hold(bool on);
void board_rem(bool on);
void board_led(bool on);
bool board_hold_get(void);
bool board_rem_get(void);
/* SW1: bit 0 = key 4 (read serial number), bits 1..3 = keys 3..1 (type); a set bit is a closed key */
uint8_t board_switch(void);
