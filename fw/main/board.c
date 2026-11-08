#include "board.h"

/* HOLD and REM pads are outputs with the input buffer off, so the last written level is kept here */
static bool s_hold, s_rem;

void board_init(void)
{
    /* HOLD first: the supply stays on even if the light goes during start; REM stays off until the DAC is up */
    gpio_set_level(PIN_HOLD, 1);
    gpio_set_level(PIN_REM_EN, 0);
    gpio_set_level(PIN_LED, 0);
    s_hold = true;
    s_rem = false;
    gpio_config_t out = {
        .pin_bit_mask = BIT64(PIN_HOLD) | BIT64(PIN_REM_EN) | BIT64(PIN_LED),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    /* slow edges on everything that is not an I2S data line: less radiation (schematic note) */
    gpio_set_drive_capability(PIN_HOLD, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(PIN_REM_EN, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(PIN_LED, GPIO_DRIVE_CAP_0);

    /* OS8104A reset released: after its power-on reset it bypasses the ring by itself */
    gpio_set_level(PIN_MOST_RST, 1);
    gpio_config_t od = {
        .pin_bit_mask = BIT64(PIN_MOST_RST),
        .mode = GPIO_MODE_OUTPUT_OD,
    };
    gpio_config(&od);
    gpio_set_drive_capability(PIN_MOST_RST, GPIO_DRIVE_CAP_0);
    gpio_set_drive_capability(PIN_TXD0, GPIO_DRIVE_CAP_0);

    /* inputs with external pull-ups: the switch (input-only pins) and the OS8104A interrupt */
    gpio_config_t in = {
        .pin_bit_mask = BIT64(PIN_MODE0) | BIT64(PIN_MODE1) | BIT64(PIN_MODE2) | BIT64(PIN_MODE3) | BIT64(PIN_MOST_INT),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&in);
    /* the ring clocks: after its reset the OS8104A keeps FSY and SCK as inputs until bSDC1.IO is set (DS8104AFP4 7.2),
       and only SCK has a pull-down on the board (R16), so both lines get the pad pull-down until the ring drives them */
    gpio_config_t clk = {
        .pin_bit_mask = BIT64(PIN_BCK) | BIT64(PIN_LRCK),
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
    };
    gpio_config(&clk);
}

void board_hold(bool on) { s_hold = on; gpio_set_level(PIN_HOLD, on); }
void board_rem(bool on)  { s_rem = on; gpio_set_level(PIN_REM_EN, on); }
void board_led(bool on)  { gpio_set_level(PIN_LED, on); }
bool board_hold_get(void) { return s_hold; }
bool board_rem_get(void)  { return s_rem; }

uint8_t board_switch(void)
{
#if CONFIG_ETH_USE_OPENETH
    return 0;           /* QEMU: its inputs read 0, as if every key were closed - no switch there, all keys open */
#endif
    return (!gpio_get_level(PIN_MODE0) << 0) | (!gpio_get_level(PIN_MODE1) << 1) |
           (!gpio_get_level(PIN_MODE2) << 2) | (!gpio_get_level(PIN_MODE3) << 3);
}
