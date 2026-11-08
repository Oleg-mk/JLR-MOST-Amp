/* see phone.h. One task feeds (the Bluetooth stack's, the AirPlay receiver's or the virtual phone's), another pulls
   (the sound engine on core 1): the FIFO is single-producer single-consumer. A new stream takes both locks - the
   feeders' and the one the puller only tries, so a pull never waits */
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "phone.h"
#include "src.h"

#define CAP         8192                    /* FIFO frames, a power of two: 186 ms at 44.1 kHz */
#define HALF        (SRC_TAPS / 2)
#define TARGET_S    0.100                   /* the fill the rate control keeps: room for A2DP's bursts both ways */
#define AVG_S       2.0                     /* the fill is smoothed twice over this before the control sees it: once
                                               let A2DP's bursts through as a wobble of 10-30 ppm per block */
#define WN          (2 * M_PI * 0.01)       /* control loop at 0.01 Hz, critically damped: the pitch moves by ppm */
#define KP          (2 * WN)
#define KI          (WN * WN)
#define CORR_MAX    1000e-6                 /* phones and rings keep within a few hundred ppm */
#define HEADROOM    0.70795f                /* -3 dB: interpolation can overshoot a full-scale recording */
#define FADE_IN     480                     /* frames: the sound fades in over 10 ms whenever playing (re)starts */
#define DECLICK     96                      /* frames: after an underrun the last frame ramps out over 2 ms, no step */

static int16_t *s_buf;                  /* (CAP + SRC_TAPS) frames, internal RAM from the heap: with Bluetooth on, the
                                           static data segment has no room for it. Frames 0..SRC_TAPS-1 repeat after
                                           the end, so every window is in one piece */
static uint32_t s_w;                    /* frames written, ever (the feeder's) */
static uint32_t s_r;                    /* the read position's whole frames (the puller's) */
static uint32_t s_frac;                 /* and its fraction */
static volatile int s_rate;
static volatile phone_src_t s_src;      /* who owns the stream: one source plays at a time, the latest to start */
static volatile uint32_t s_gen;         /* the streams started, ever: each one's number */
static bool s_playing;
static uint32_t s_fade;                 /* frames played since playing (re)started, up to FADE_IN */
static float s_tail[2];                 /* the last frame played, and how much of it still ramps out after an underrun */
static int s_tail_n;
static double s_step0, s_avg1, s_avg, s_int, s_corr;
static volatile float s_gain = 1;       /* the phone's own volume, linear; s_gain_now follows it block by block */
static float s_gain_now = 1;
static phone_stats_t s_st;
static SemaphoreHandle_t s_lock, s_feed;
static phone_remote_fn s_remote[PHONE_VIRTUAL + 1];
static QueueHandle_t s_ctl;
static const char *const s_name[] = { "-", "Bluetooth", "AirPlay", "virtual phone" };

/* the requests to pause and resume, one after another: a source's call may wait on the network or the radio */
static void ctl_task(void *arg)
{
    uint8_t r[2];
    for (;;) {
        if (xQueueReceive(s_ctl, r, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI("phone", "%s: %s", s_name[r[0]], r[1] == PHONE_PAUSE ? "pause" : "play");
        if (s_remote[r[0]]) s_remote[r[0]](r[1]);
        else ESP_LOGW("phone", "%s takes no remote control", s_name[r[0]]);
    }
}

void phone_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_feed) s_feed = xSemaphoreCreateMutex();
    if (!s_buf) s_buf = heap_caps_malloc(sizeof *s_buf * (CAP + SRC_TAPS) * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buf) ESP_LOGE("phone", "no internal RAM for the FIFO");
    if (!s_ctl) {
        s_ctl = xQueueCreate(8, 2);
        xTaskCreate(ctl_task, "phone_ctl", 4096, NULL, 4, NULL);
    }
}

void phone_remote_set(phone_src_t src, phone_remote_fn fn)
{
    phone_init();
    if (src > PHONE_NONE && src <= PHONE_VIRTUAL) s_remote[src] = fn;
}

void phone_remote(phone_src_t src, phone_cmd_t cmd)
{
    uint8_t r[2] = { src, cmd };
    if (s_ctl && src > PHONE_NONE && src <= PHONE_VIRTUAL) xQueueSend(s_ctl, r, 0);
}

phone_src_t phone_owner(uint32_t *gen)
{
    if (!s_rate) {
        if (gen) *gen = 0;
        return PHONE_NONE;
    }
    if (gen) *gen = s_gen;
    return s_src;
}

void phone_start(phone_src_t src, int rate)
{
    phone_init();
    xSemaphoreTake(s_feed, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    phone_src_t old = s_rate ? s_src : PHONE_NONE;
    s_rate = 0;
    s_src = PHONE_NONE;
    if (s_buf && src_design(rate, PHONE_OUT_RATE) == 0) {
        s_w = 0;
        s_r = HALF - 1;                 /* the first window is the first SRC_TAPS frames */
        s_frac = 0;
        s_playing = false;
        s_tail_n = 0;
        s_avg1 = s_avg = s_int = s_corr = 0;
        s_step0 = (double)rate / PHONE_OUT_RATE;
        memset(&s_st, 0, sizeof s_st);
        s_gain = s_gain_now = 1;        /* a new source starts at full; AirPlay sets its volume after */
        s_gen++;
        s_src = src;
        s_rate = rate;
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_feed);
    if (old != PHONE_NONE && old != src) phone_remote(old, PHONE_PAUSE);    /* the newer plays, the older waits */
}

void phone_stop(phone_src_t src)
{
    phone_init();
    xSemaphoreTake(s_feed, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_src == src) {
        s_rate = 0;
        s_playing = false;
        s_src = PHONE_NONE;
    }
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_feed);
}

bool phone_active(void) { return s_rate != 0; }

void phone_gain(float db) { s_gain = db <= -144 ? 0 : powf(10, (db > 0 ? 0 : db) / 20); }

void phone_feed(phone_src_t src, const int16_t *lr, int frames)
{
    if (s_src != src || !s_rate) return;
    xSemaphoreTake(s_feed, portMAX_DELAY);
    if (s_src != src) {                 /* another source took over meanwhile */
        xSemaphoreGive(s_feed);
        return;
    }
    uint32_t w = s_w, start = __atomic_load_n(&s_r, __ATOMIC_ACQUIRE) - (HALF - 1);
    int room = CAP - (int)(w - start);
    if (frames > room) {
        s_st.overruns++;
        frames = room > 0 ? room : 0;
    }
    for (int i = 0; i < frames; i++, w++) {
        uint32_t j = w & (CAP - 1);
        s_buf[2 * j] = lr[2 * i];
        s_buf[2 * j + 1] = lr[2 * i + 1];
        if (j < SRC_TAPS) {
            s_buf[2 * (j + CAP)] = lr[2 * i];
            s_buf[2 * (j + CAP) + 1] = lr[2 * i + 1];
        }
    }
    __atomic_store_n(&s_w, w, __ATOMIC_RELEASE);
    s_st.in_frames += frames;
    xSemaphoreGive(s_feed);
}

static inline int32_t q31(float x)
{
    x *= 2147483648.0f;
    return x >= 2147483520.0f ? 2147483520 : x <= -2147483648.0f ? INT32_MIN : (int32_t)x;
}

void phone_pull(int32_t *out, int n)
{
    if (!s_rate || xSemaphoreTake(s_lock, 0) != pdTRUE) {      /* no stream, or a new one being set up */
        if (out) memset(out, 0, sizeof *out * 2 * n);
        return;
    }
    uint32_t w = __atomic_load_n(&s_w, __ATOMIC_ACQUIRE), r = s_r;
    double fill = (double)(int32_t)(w - r) / s_rate;          /* seconds of sound ahead of the read position */
    if (!s_playing && fill >= TARGET_S) {
        s_playing = true;
        s_fade = 0;
        s_avg1 = s_avg = fill;
    }
    if (s_playing) {
        /* once per block: the smoothed fill against the target; the integral holds the clocks' difference */
        double dt = (double)n / PHONE_OUT_RATE;
        s_avg1 += (fill - s_avg1) * (dt / AVG_S);
        s_avg += (s_avg1 - s_avg) * (dt / AVG_S);
        double e = s_avg - TARGET_S, i = s_int + e * dt, c = KP * e + KI * i;
        if (c > CORR_MAX) c = CORR_MAX;
        else if (c < -CORR_MAX) c = -CORR_MAX;
        else s_int = i;
        s_corr = c;
    }
    uint64_t step = (uint64_t)llround(s_step0 * (1 + s_corr) * 4294967296.0);
    float g = s_gain_now * HEADROOM, dg = (s_gain - s_gain_now) * HEADROOM / n;    /* a new volume glides over the block */
    for (int i = 0; i < n; i++, g += dg) {
        if (s_playing && (int32_t)(w - r) < HALF + 1) {     /* the window's last frame is not there: prefill again */
            s_playing = false;
            s_st.underruns++;
            s_tail_n = DECLICK;
        }
        if (!s_playing) {
            float t = s_tail_n ? (float)s_tail_n-- / DECLICK : 0;
            if (out) {
                out[2 * i] = q31(s_tail[0] * t);
                out[2 * i + 1] = q31(s_tail[1] * t);
            }
            continue;
        }
        if (out) {
            float y[2], f = s_fade < FADE_IN ? (float)s_fade++ / FADE_IN : 1.0f;
            src_frame(&s_buf[2 * ((r - (HALF - 1)) & (CAP - 1))], s_frac, y);
            s_tail[0] = y[0] * g * f;
            s_tail[1] = y[1] * g * f;
            out[2 * i] = q31(s_tail[0]);
            out[2 * i + 1] = q31(s_tail[1]);
        }
        uint64_t p = (uint64_t)s_frac + (uint32_t)step;
        s_frac = (uint32_t)p;
        r += (uint32_t)(step >> 32) + (uint32_t)(p >> 32);
    }
    s_gain_now = s_gain;
    __atomic_store_n(&s_r, r, __ATOMIC_RELEASE);
    s_st.out_frames += n;
    s_st.fill_ms = (float)((s_playing ? s_avg : fill) * 1000);
    s_st.corr_ppm = (float)(s_corr * 1e6);
    s_st.playing = s_playing;
    xSemaphoreGive(s_lock);
}

void phone_stats(phone_stats_t *st)
{
    *st = s_st;
    st->rate = s_rate;
    st->src = s_src;
}
