#pragma once
/* PCM5122 DAC on I2C1 (SLAS763C): SCK = MCLK 12.288 MHz from the OS8104A RMCK, BCK and LRCK from the ring, DIN = DOUT.
   XSMT is held high by R31, so muting is by register only */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* page 0 registers used here */
#define PCM_RESET   0x01    /* RSTM b4, RSTR b0 */
#define PCM_STANDBY 0x02    /* RQST b4 standby, RQPD b0 powerdown */
#define PCM_MUTE    0x03    /* RQML b4, RQMR b0: soft mute with the volume ramp */
#define PCM_PLL     0x04    /* PLCK b4 (1 = unlocked), PLLE b0 */
#define PCM_FORMAT  0x28    /* AFMT b5:4, ALEN b1:0 */
#define PCM_VOLCTL  0x3C    /* PCTL b1:0 */
#define PCM_VOLL    0x3D    /* 30h = 0 dB, +1 = -0.5 dB, FFh = mute */
#define PCM_RATE    0x5B    /* detected: DTFS b6:4 sample rate, DTSR b3:0 SCK ratio */
#define PCM_CLKSTAT 0x5E    /* 0 = ok: CDST b6 SCK, b4 LRCK/BCK, b3 fS/SCK, b2 SCK, b1 BCK, b0 fS */
#define PCM_CLKERR  0x5F    /* LTSH b4, CKMF b2, CSRF b1, CERF b0 */
#define PCM_AMUTE   0x6C    /* AMLM b1, AMRM b0: 1 = analog output on */
#define PCM_POWER   0x76    /* BOTM b7, PSTM b3:0: 5 = playing */

esp_err_t pcm5122_init(void);      /* I2C1, then the DAC set up and muted; it plays as soon as the ring clocks run */
esp_err_t pcm5122_setup(void);     /* the set-up again (registers only); ESP_ERR_INVALID_STATE before pcm5122_init */
bool pcm5122_ready(void);           /* the last set-up went through */
esp_err_t pcm5122_mute(bool on);
esp_err_t pcm5122_read(uint8_t reg, uint8_t *v);
