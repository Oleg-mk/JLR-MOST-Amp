/* see music.h. Runs in the sound engine's task once per block, so its clocks count frames; a phone is paused or
   resumed through phone_remote(), which calls the source from a task of its own */
#include <math.h>
#include <string.h>
#include "music.h"
#include "phone.h"

#define RATE        PHONE_OUT_RATE
#define MS(x)       ((uint32_t)(x) * (RATE / 1000))
#define ENV_S       0.1f        /* levels: RMS smoothed over this */
#define QUIET_DB    (-60.0f)    /* below: silence - an idle AUX input's noise is far lower, music hardly ever */
#define LOUD_DB     (-50.0f)    /* above: music; in between the state holds */
#define LOUD_MS     300         /* music this long: the head unit plays */
#define QUIET_MS    2000        /* silence this long: the head unit is silent and the phone may play */
#define RESUME_MS   5000        /* and this long resumes the phone paused for the head unit: longer than a CD's pause
                                   between tracks (2 s) or a radio's retuning, so neither brings the phone in */
#define CHANGED_WIN 10000       /* QUIET_MS is enough if the head unit connected, disconnected or (un)muted sink 1
                                   within this, as at a source change */
#define HEARD_DB    (-70.0f)    /* a phone sounding within HEARD_MS before it is paused is resumed later; one its owner
                                   paused, streaming its last seconds of silence, is not */
#define HEARD_MS    2000
#define GRACE_MS    1000        /* a stream starting while the head unit plays is paused once it sounds or after this */
#define FADE_MS     30          /* the head unit's music and the phone's cross over this */
#define DECLICK     96          /* frames: the step a cut-off phone stream leaves ramps out over 2 ms */

enum { HU_UNKNOWN, HU_SILENT, HU_PLAYING };

static int s_hu;
static float s_hu_ms, s_ph_ms;          /* mean squares, full scale 1 */
static uint32_t s_quiet, s_loud, s_heard, s_changed, s_age;    /* frames */
static volatile bool s_change;
static float s_g;                       /* 0 the head unit, 1 the phone */
static phone_src_t s_was;               /* the stream of the last block: its source, 0 none */
static uint32_t s_gen, s_done;          /* and generation; the last one paused */
static phone_src_t s_paused;            /* paused for the head unit, to be resumed */
static uint32_t s_paused_gen;
static float s_last[2], s_tail[2];      /* the phone's part of the last frame; what of a cut-off one still ramps out */
static int s_tail_n;

static inline uint32_t add(uint32_t a, uint32_t b) { return a > UINT32_MAX - b ? UINT32_MAX : a + b; }

static float mean_sq(const int32_t *x, int n)
{
    float acc = 0;
    for (int i = 0; i < 2 * n; i++) {
        float v = x[i] * (1.0f / 2147483648.0f);
        acc += v * v;
    }
    return acc / (2 * n);
}

static float db(float ms) { return 10 * log10f(ms + 1e-20f); }

void music_init(void)
{
    s_hu = HU_UNKNOWN;
    s_hu_ms = s_ph_ms = 0;
    s_quiet = s_loud = s_age = 0;
    s_heard = s_changed = UINT32_MAX;
    s_change = false;
    s_g = 0;
    s_was = s_paused = PHONE_NONE;
    s_gen = s_done = 0;
    s_last[0] = s_last[1] = 0;
    s_tail_n = 0;
}

void music_hu_changed(void) { s_change = true; }

void music_block(const int32_t *hu, int32_t *out, int n)
{
    static int32_t ph[2 * MUSIC_MAX];
    if (n > MUSIC_MAX) n = MUSIC_MAX;
    float a = n / (ENV_S * RATE);
    if (a > 1) a = 1;

    /* the head unit: playing or silent, with hysteresis in level and in time */
    if (s_change) {
        s_change = false;
        s_changed = 0;
    } else {
        s_changed = add(s_changed, n);
    }
    s_hu_ms += ((hu ? mean_sq(hu, n) : 0) - s_hu_ms) * a;
    float hu_db = db(s_hu_ms);
    s_quiet = hu_db < QUIET_DB ? add(s_quiet, n) : 0;
    s_loud = hu_db >= LOUD_DB ? add(s_loud, n) : 0;
    if (s_hu != HU_PLAYING && s_loud >= MS(LOUD_MS)) s_hu = HU_PLAYING;
    else if (s_hu != HU_SILENT && s_quiet >= MS(QUIET_MS)) s_hu = HU_SILENT;

    /* the phone: pulled whenever it streams, heard or not, so its rate control keeps following */
    uint32_t gen;
    phone_src_t who = phone_owner(&gen);
    if (who != PHONE_NONE) {
        phone_pull(ph, n);
        s_ph_ms += (mean_sq(ph, n) - s_ph_ms) * a;
    } else {
        memset(ph, 0, sizeof *ph * 2 * n);
        s_ph_ms = 0;
    }
    s_heard = db(s_ph_ms) >= HEARD_DB ? 0 : add(s_heard, n);
    if (who != s_was || gen != s_gen) {
        if (s_g > 0) {                  /* the stream heard was replaced or ended: no step where it stopped */
            s_tail[0] = s_last[0];
            s_tail[1] = s_last[1];
            s_tail_n = DECLICK;
        }
        s_was = who;
        s_gen = gen;
        s_age = 0;
    } else {
        s_age = add(s_age, n);
    }

    /* the head unit plays: a phone streaming is paused, once per stream - at once if it sounds, else after the grace -
       and resumed later only if it sounded */
    if (s_hu == HU_PLAYING && who != PHONE_NONE && gen != s_done && (!s_heard || s_age >= MS(GRACE_MS))) {
        s_done = gen;
        phone_remote(who, PHONE_PAUSE);
        if (s_heard < MS(HEARD_MS)) {
            s_paused = who;
            s_paused_gen = gen;
        }
    }
    /* the head unit silent long enough: the phone paused for it plays again, unless another stream is on by now */
    if (s_paused != PHONE_NONE && s_hu == HU_SILENT &&
        s_quiet >= MS(s_changed < MS(CHANGED_WIN) ? QUIET_MS : RESUME_MS)) {
        if (who == PHONE_NONE || (who == s_paused && gen == s_paused_gen)) phone_remote(s_paused, PHONE_PLAY);
        s_paused = PHONE_NONE;
    }

    /* sink 1: the head unit's frames as they are, the phone's, or a crossing between them */
    float target = who != PHONE_NONE && s_hu == HU_SILENT ? 1.0f : 0.0f;
    if (s_g == target && !s_tail_n) {
        if (target) memcpy(out, ph, sizeof *out * 2 * n);
        else if (!hu) memset(out, 0, sizeof *out * 2 * n);
        else if (out != hu) memcpy(out, hu, sizeof *out * 2 * n);
    } else {
        const float step = 1.0f / MS(FADE_MS);
        for (int i = 0; i < n; i++) {
            s_g = s_g < target ? fminf(target, s_g + step) : fmaxf(target, s_g - step);
            float t = s_tail_n ? (float)s_tail_n-- / DECLICK : 0;
            for (int c = 0; c < 2; c++) {
                float h = hu ? (float)hu[2 * i + c] : 0, y = h + ((float)ph[2 * i + c] - h) * s_g + s_tail[c] * t;
                out[2 * i + c] = y >= 2147483520.0f ? 2147483520 : y <= -2147483648.0f ? INT32_MIN : (int32_t)y;
            }
        }
    }
    s_last[0] = ph[2 * n - 2] * s_g;
    s_last[1] = ph[2 * n - 1] * s_g;
}

void music_stats(music_stats_t *st)
{
    st->hu = s_hu;
    st->hu_db = db(s_hu_ms);
    st->phone = s_g > 0.5f;
    st->paused = s_paused;
}
