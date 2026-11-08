/* PCM5122 driver. The DAC runs straight from SCK: at 48 kHz 256Fs needs no PLL (SLAS763C table 32), so the PLL is off
   and its jitter stays out of the DAC. Levels are set in the ESP32, the DAC volume stays at 0 dB */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "board.h"
#include "pcm5122.h"

static const char *TAG = "pcm5122";
static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, sizeof b, 50);
}

esp_err_t pcm5122_read(uint8_t reg, uint8_t *v)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 50);
}

static volatile bool s_ready;

bool pcm5122_ready(void) { return s_ready; }

esp_err_t pcm5122_mute(bool on)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return wr(PCM_MUTE, on ? 0x11 : 0x00);
}

esp_err_t pcm5122_init(void)
{
    if (s_dev) return pcm5122_setup();
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PIN_DAC_SDA,
        .scl_io_num = PIN_DAC_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "I2C1 bus");
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_PCM5122,
        .scl_speed_hz = 400000,     /* R27, R28 4.7k */
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dc, &s_dev), TAG, "PCM5122 device");
    return pcm5122_setup();
}

/* the registers alone, so a failed set-up can be tried again (power.c) without a second bus */
esp_err_t pcm5122_setup(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    /* register reset is allowed in standby only (SLAS763C register 1) */
    static const uint8_t reset[][2] = {
        { 0x00, 0x00 },             /* page 0 */
        { PCM_STANDBY, 0x10 },
        { PCM_RESET, 0x11 },        /* modules and registers */
    };
    static const uint8_t setup[][2] = {
        { PCM_STANDBY, 0x10 },      /* the register reset cleared the standby request: back to it while setting up */
        { PCM_MUTE, 0x11 },         /* muted until the firmware has sound */
        { PCM_PLL, 0x00 },          /* PLL off: the master clock is SCK */
        { PCM_FORMAT, 0x03 },       /* I2S, 32-bit words: the ESP32 slots */
        { PCM_VOLCTL, 0x01 },       /* right volume follows left */
        { PCM_VOLL, 0x30 },         /* 0 dB */
        { PCM_STANDBY, 0x00 },      /* run; with no valid clocks the DAC waits in standby by itself */
    };
    esp_err_t e = ESP_OK;
    for (size_t i = 0; i < sizeof reset / sizeof reset[0] && e == ESP_OK; i++) e = wr(reset[i][0], reset[i][1]);
    vTaskDelay(pdMS_TO_TICKS(2));
    for (size_t i = 0; i < sizeof setup / sizeof setup[0] && e == ESP_OK; i++) e = wr(setup[i][0], setup[i][1]);
    if (e != ESP_OK) ESP_LOGE(TAG, "no answer on I2C1: %s", esp_err_to_name(e));
    else ESP_LOGI(TAG, "set up: SCK clock, I2S 32-bit, muted");
    s_ready = e == ESP_OK;
    return e;
}
