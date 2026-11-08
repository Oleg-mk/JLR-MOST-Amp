/* The sound engine: both I2S controllers as slaves of the ring clocks. In: the head unit's sinks the OS8104A routes
   (stage 4). Out of both: sink 1's music or the phone's in its place (music.c) through the sound processing, or on the
   bench a test tone or the bit-exact pattern. Peak meters on both inputs */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "board.h"
#include "audio.h"
#include "dsp.h"
#include "music.h"
#include "os8104.h"

static const char *TAG = "audio";

#define RATE     48000
#define FRAMES   192        /* per DMA buffer: 4 ms */
#define TOUT_MS  100        /* a write or read that long means no ring clock */

static i2s_chan_handle_t s_tx[2], s_rx[2];
static volatile bool s_live[DSP_SINKS]; /* the sound processing's sinks with ring channels routed: 1 stereo, 2..4 mono */
static volatile float s_step, s_amp;    /* tone: phase step in radians per frame, amplitude with 1 = full scale */
static audio_stats_t s_st;
static volatile bool s_check, s_check_reset;
static audio_check_t s_chk[2];
static uint32_t s_prev[2];
static uint8_t s_sync[2];               /* 0: nothing yet, 1: one frame seen, 2: the pattern confirmed, counting */

/* x^32 + x^22 + x^2 + x + 1, a maximal 32-bit LFSR (period 2^32 - 1, about 25 h at 48 kHz) */
static inline uint32_t lfsr_next(uint32_t x) { return (x >> 1) ^ (-(x & 1u) & 0x80200003u); }

/* counting starts after two frames in a row fit the pattern: the tone still queued in the DMA and the silence before
   the loop delivers are not errors, and a frame that does not fit after that is */
static void check(int p, const int32_t *b, size_t frames)
{
    audio_check_t *c = &s_chk[p];
    for (size_t i = 0; i < frames; i++) {
        uint32_t l = b[2 * i], r = b[2 * i + 1], e = lfsr_next(s_prev[p]);
        bool fits = s_sync[p] && l == e && r == ~e;
        if (s_sync[p] == 2) {
            c->frames++;
            if (!fits) {
                if (!c->errors) { c->exp = e; c->got_l = l; c->got_r = r; }
                c->errors++;
            }
        } else if (!l && !r) {
            c->silent++;
        } else {
            s_sync[p] = fits ? 2 : 1;
            c->got_l = l;       /* until the pattern is found: the last frame, to see a shift or a swap */
            c->got_r = r;
        }
        s_prev[p] = l;
    }
}

static esp_err_t controller(i2s_port_t port, gpio_num_t dout, gpio_num_t din)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(port, I2S_ROLE_SLAVE);
    cc.dma_desc_num = 4;
    cc.dma_frame_num = FRAMES;
    cc.auto_clear_after_cb = true;  /* an output starved for a moment (a flash write) sends zeros, not its last 16 ms
                                       over and over */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &s_tx[port], &s_rx[port]), TAG, "I2S%d", port);
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = PIN_BCK, .ws = PIN_LRCK, .dout = dout, .din = din },
    };
    /* the slave samples BCK with its own clock at 16x (8 is the minimum): TX data follows the BCK edge within ~20 ns
       and the OS8104A and PCM5122 keep most of the half period as setup */
    sc.clk_cfg.bclk_div = 16;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx[port], &sc), TAG, "I2S%d TX", port);
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx[port], &sc), TAG, "I2S%d RX", port);
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx[port]), TAG, "I2S%d TX on", port);
    return i2s_channel_enable(s_rx[port]);
}

/* All four directions run on the same BCK and LRCK, so one task takes a block from both inputs and gives a block to
   both outputs, and they stay in step: the ring's sound is 4 ms of input plus the queued output late */
static void sound_task(void *arg)
{
    static int32_t in[2][FRAMES * 2], buf[FRAMES * 2], pat[FRAMES * 2], mono[3][FRAMES];
    float ph = 0;
    uint32_t s = 1;
    esp_task_wdt_add(NULL);                 /* a loop is 4 ms, or 100 ms per read with no ring clock */
    for (;;) {
        esp_task_wdt_reset();
        if (s_check_reset) {
            memset(s_chk, 0, sizeof s_chk);
            memset(s_sync, 0, sizeof s_sync);
            s_check_reset = false;
        }
        bool got[2];
        for (int p = 0; p < 2; p++) {
            size_t n = 0;
            got[p] = i2s_channel_read(s_rx[p], in[p], sizeof in[p], &n, TOUT_MS) == ESP_OK && n == sizeof in[p];
            if (!got[p]) {
                s_st.rx_timeouts[p]++;
                continue;
            }
            for (size_t i = 0; i < n / sizeof in[p][0]; i++) {
                uint32_t a = in[p][i] < 0 ? 0u - (uint32_t)in[p][i] : (uint32_t)in[p][i];
                if (a > s_st.peak[p][i & 1]) s_st.peak[p][i & 1] = a;
            }
            s_st.rx_frames[p] += FRAMES;
            if (s_check) check(p, in[p], FRAMES);
        }
        bool chk = s_check;
        /* the largest float below 2^31; DOUT stays silent during the check */
        float step = s_step, amp = chk ? 0 : s_amp * 2147483520.0f;
        if (chk || amp > 0) {                   /* the bench: the pattern to SR1, a tone (or silence) out of both */
            for (int i = 0; i < FRAMES; i++) {
                buf[2 * i] = buf[2 * i + 1] = (int32_t)(amp * sinf(ph));
                ph += step;
                if (ph >= 2 * (float)M_PI) ph -= 2 * (float)M_PI;
                if (chk) {
                    pat[2 * i] = (int32_t)s;
                    pat[2 * i + 1] = (int32_t)~s;
                    s = lfsr_next(s);
                }
            }
        } else {
            /* SX3's slot L carries mono 0 in its top half and mono 1 below, slot R mono 2 on top (os8104.h); each
               goes on left-justified as the sound processing takes it */
            const int32_t *mo[3] = { NULL, NULL, NULL };
            for (int k = 0; k < 3; k++) {
                if (!s_live[k + 1] || !got[1]) continue;
                for (int i = 0; i < FRAMES; i++) {
                    uint32_t w = (uint32_t)in[1][2 * i + (k == 2)];
                    mono[k][i] = (int32_t)(k == 1 ? w << 16 : w & 0xFFFF0000u);
                }
                mo[k] = mono[k];
            }
            /* sink 1: the ring's stereo as it came (16-bit samples on top of 32-bit slots) or the phone's */
            music_block(s_live[0] && got[0] ? in[0] : NULL, buf, FRAMES);
            dsp_process(buf, mo, buf, FRAMES);
        }
        size_t n;
        if (i2s_channel_write(s_tx[0], chk ? pat : buf, sizeof buf, &n, TOUT_MS) != ESP_OK) s_st.tx_timeouts++;
        if (i2s_channel_write(s_tx[1], buf, sizeof buf, &n, TOUT_MS) != ESP_OK) s_st.tx_timeouts++;
        s_st.frames += FRAMES;
    }
}

bool audio_sink(int dsp, const uint8_t *ch, int n)
{
    if (dsp < 1 || dsp > DSP_SINKS) return false;
    uint8_t m[4];
    const uint8_t *r = NULL;
    bool on;
    if (dsp == 1) {
        /* 4 channels and more (AUD3's video brings 6, its first 4 the stereo, as usb2most takes them); a mono
           connection to sink 1 goes into both slots */
        r = ch && n >= 4 ? ch : NULL;
        if (!r && ch && n >= 2) {
            m[0] = m[2] = ch[0];
            m[1] = m[3] = ch[1];
            r = m;
        }
        on = r != NULL;
    } else {
        on = ch && n >= 2;
        r = on ? ch : NULL;
    }
    esp_err_t e = ESP_OK;
    for (int t = 0; t < 2; t++) {           /* one I2C error must not cost the sink: once again */
        e = dsp == 1 ? os8104_route_stereo(r) : os8104_route_mono(dsp - 2, r);
        if (e == ESP_OK) break;
    }
    s_live[dsp - 1] = on && e == ESP_OK;    /* a route that did not take plays nothing, not the channels before */
    if (e != ESP_OK) ESP_LOGW(TAG, "sink %d of the processing: not routed (%s)", dsp, esp_err_to_name(e));
    return e == ESP_OK;
}

void audio_check(bool on)
{
    if (on) s_check_reset = true;   /* off keeps the counts for reading */
    s_check = on;
}

void audio_check_stats(audio_check_t st[2])
{
    memcpy(st, s_chk, sizeof s_chk);
}

void audio_tone(float hz, float dbfs)
{
    if (hz <= 0 || hz >= RATE / 2) {
        s_amp = 0;
        return;
    }
    s_step = 2 * (float)M_PI * hz / RATE;
    s_amp = powf(10, (dbfs > 0 ? 0 : dbfs) / 20);
}

void audio_stats(audio_stats_t *st)
{
    *st = s_st;
    memset(s_st.peak, 0, sizeof s_st.peak);
}

uint32_t audio_frames(void) { return s_st.frames; }

esp_err_t audio_init(void)
{
    ESP_RETURN_ON_ERROR(controller(I2S_NUM_0, PIN_SPDIF_SRC, PIN_MOST_SX1), TAG, "I2S0");
    ESP_RETURN_ON_ERROR(controller(I2S_NUM_1, PIN_DOUT, PIN_MOST_SX3), TAG, "I2S1");
    /* core 1 is the sound core (docs/firmware.md) */
    if (xTaskCreatePinnedToCore(sound_task, "sound", 4096, NULL, 20, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the sound task");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "I2S0 and I2S1 slaves on BCK IO15, LRCK IO4; the head unit's sinks as it connects them");
    return ESP_OK;
}
