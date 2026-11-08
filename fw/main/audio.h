#pragma once
/* Both I2S controllers are slaves of the ring clocks: BCK 64Fs (IO15) and LRCK 48 kHz (IO4) from the OS8104A, 32-bit
   stereo slots, Philips format. I2S0: TX -> SR1 (S/PDIF source), RX <- SX1 (the stereo sink). I2S1: TX -> DOUT
   (PCM5122, HDMI), RX <- SX3 (the mono sinks). Out of both: the sound processing's output, or on the bench a test
   tone; peak meters on both RX */
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t frames;            /* frames written to both TX */
    uint32_t tx_timeouts;       /* 100 ms with no clock */
    uint32_t rx_frames[2];      /* per controller */
    uint32_t rx_timeouts[2];
    uint32_t peak[2][2];        /* [controller][left, right], absolute sample value, since the last read */
} audio_stats_t;

/* bit-exact check: SR1 gets a 32-bit LFSR, left = s, right = ~s, one step per frame; each received frame must be the
   next step of the one before, so no delay search is needed and a flipped bit, a lost frame or swapped channels all
   count as errors */
typedef struct {
    uint32_t frames;            /* frames checked */
    uint32_t errors;
    uint32_t silent;            /* all-zero frames: nothing came back yet */
    uint32_t exp, got_l, got_r; /* the first error: expected left, received left and right; before the pattern is
                                   found got_l, got_r hold the last non-silent frame */
} audio_check_t;

esp_err_t audio_init(void);
/* stage 4: sink dsp of the sound processing (1 stereo, 2..4 mono) onto the ring channels ch[0..n) the head unit
   connected, or off with ch NULL; the OS8104A routes them, the engine takes the sink in from the next block. False:
   the chip did not take the route (I2C, tried twice) and the sink plays nothing */
bool audio_sink(int dsp, const uint8_t *ch, int n);
void audio_tone(float hz, float dbfs);      /* hz <= 0: the sinks' sound again; > 0 a tone instead */
void audio_stats(audio_stats_t *st);        /* copies the counters and clears the peaks */
uint32_t audio_frames(void);                /* frames written to TX, ever: it moves while the sound task goes round */
void audio_check(bool on);                  /* on: the pattern to SR1, DOUT silent, counters cleared */
void audio_check_stats(audio_check_t st[2]);    /* [I2S0 <- SX1, I2S1 <- SX3] */
