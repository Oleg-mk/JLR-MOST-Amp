/* see dsp.h. The control side (the amplifier emulation, core 0) only writes targets; dsp_process (the sound engine,
   core 1) takes them at the start of a block, so no lock is needed: every target is one aligned 32-bit word or less.
   After the sinks' sum: the balance, then the output gate */
#include <math.h>
#include <string.h>
#include "dsp.h"

#define GLIDE_MS        10          /* a new level slides in over this, so no zipper noise, and lands exactly */
#define FADE_MS         20          /* mute fades while no MuteParameters came */
#define DEFAULT_DB      (-20.0f)    /* a sink's level until the head unit sets one: audible, never full */
#define TONE_DB_STEP    2.0f        /* one step of Bass or Treble; the amplifier's own scale is open (stock_amps.md 8) */
#define TONE_GLIDE_DB   1.0f        /* per block, so a tone change does not click */
#define BASS_HZ         100.0f
#define TREBLE_HZ       10000.0f
#define BAL_DB_STEP     2.0f        /* one step of Balance, -10..+10 (stock_amps.md 5): 20 dB off one side at the end.
                                       An assumption until heard against the stock amplifier */
#define BAL_STEPS       10
#define FULL            2147483520.0f   /* the largest float below 2^31 */

typedef struct {
    volatile float to_db[2];        /* targets from the control side: the level for the left and the right output.
                                       Two words: a block may take one new and one old, and the next block mends it
                                       within the same glide */
    volatile bool to_mute;
    volatile uint16_t in_ms, out_ms;
    volatile uint8_t shape_in, shape_out;
    float g[2], g_to[2], g_step[2]; /* level, linear, per output: glides to g_to in g_left samples */
    int g_left;
    float m, m_from;                /* mute factor 1 (sound) .. 0 (muted), and where the current fade began */
    bool muted;                     /* the state the fade runs to */
    int fade_pos, fade_len, fade_shape;
} sink_t;

typedef struct { float a1, a2, a3, m0, m1, m2; } coef_t;
typedef struct { float ic1, ic2; } hist_t;

static int s_rate = 48000;
static sink_t s_sink[DSP_SINKS];
static volatile int8_t s_bass_to, s_treble_to, s_bal_to;
static float s_bass_db, s_treble_db;        /* where the filters are now, gliding to the targets */
static float s_bal_db, s_gl = 1, s_gr = 1;  /* the balance now, and the left and right gains it gives */
static coef_t s_cb, s_ct;
static hist_t s_hb[2], s_ht[2];             /* [left, right] */
static volatile bool s_gate_closed;         /* the output gate's target, and the time of its fade */
static volatile uint16_t s_gate_ms;
static float s_gate = 1, s_gate_to = 1, s_gate_step;
static int s_gate_left;

static float db2lin(float db) { return db <= -127 ? 0 : powf(10, db / 20); }

/* The shelves of the RBJ cookbook at slope 1 (Q = 1/sqrt 2), built as Simper's trapezoidal state-variable filter
   (Cytomic, "Linear Trapezoidal Integrated SVF", 2013) with his shelf outputs: the same response to 1e-12, but quiet in
   float. A direct form at 100 Hz feeds back at +75 dB: a 16-bit 40 Hz + 1 kHz test came out of it with float noise at
   -73 dBFS (Bass +6 dB), of this one at -131 dBFS */
static coef_t shelf(bool low, float f0, float db)
{
    float A = powf(10, db / 40), k = sqrtf(2.0f), g = tanf((float)M_PI * f0 / s_rate);
    g = low ? g / sqrtf(A) : g * sqrtf(A);
    coef_t c;
    c.a1 = 1 / (1 + g * (g + k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    if (low) {
        c.m0 = 1;
        c.m1 = k * (A - 1);
        c.m2 = A * A - 1;
    } else {
        c.m0 = A * A;
        c.m1 = k * (1 - A) * A;
        c.m2 = 1 - A * A;
    }
    return c;
}

/* at 0 dB the states still run, so switching the filter in later starts from where the sound is */
static inline float biquad(const coef_t *c, hist_t *h, float x, bool on)
{
    float v3 = x - h->ic2, v1 = c->a1 * h->ic1 + c->a2 * v3, v2 = h->ic2 + c->a2 * h->ic1 + c->a3 * v3;
    h->ic1 = 2 * v1 - h->ic1;
    h->ic2 = 2 * v2 - h->ic2;
    return on ? c->m0 * x + c->m1 * v1 + c->m2 * v2 : x;
}

void dsp_init(int rate)
{
    s_rate = rate;
    memset(s_sink, 0, sizeof s_sink);
    for (int i = 0; i < DSP_SINKS; i++) {
        sink_t *s = &s_sink[i];
        s->to_db[0] = s->to_db[1] = DEFAULT_DB;
        s->g[0] = s->g[1] = s->g_to[0] = s->g_to[1] = db2lin(DEFAULT_DB);
        s->m = 1;
        s->in_ms = s->out_ms = FADE_MS;
        s->shape_in = s->shape_out = DSP_LINEAR;
    }
    s_bass_to = s_treble_to = s_bal_to = 0;
    s_bass_db = s_treble_db = s_bal_db = 0;
    s_gl = s_gr = 1;
    s_cb = shelf(true, BASS_HZ, 0);         /* flat, but the states follow the sound from the start */
    s_ct = shelf(false, TREBLE_HZ, 0);
    memset(s_hb, 0, sizeof s_hb);
    memset(s_ht, 0, sizeof s_ht);
    s_gate_closed = false;
    s_gate_ms = 0;
    s_gate = s_gate_to = 1;
    s_gate_left = 0;
}

void dsp_balance(int steps)
{
    s_bal_to = (int8_t)(steps < -BAL_STEPS ? -BAL_STEPS : steps > BAL_STEPS ? BAL_STEPS : steps);
}

void dsp_gate(bool open, unsigned ms)
{
    s_gate_ms = ms > 60000 ? 60000 : ms;    /* the time first: a block that sees the target sees its time too */
    s_gate_closed = !open;
}

void dsp_level(int sink, float db) { dsp_level_lr(sink, db, db); }

void dsp_level_lr(int sink, float db_l, float db_r)
{
    if (sink < 1 || sink > DSP_SINKS) return;
    s_sink[sink - 1].to_db[0] = db_l;
    s_sink[sink - 1].to_db[1] = db_r;
}
void dsp_mute(int sink, bool on) { if (sink >= 1 && sink <= DSP_SINKS) s_sink[sink - 1].to_mute = on; }

void dsp_mute_params(int sink, unsigned in_ms, unsigned out_ms, int shape_in, int shape_out)
{
    if (sink < 1 || sink > DSP_SINKS) return;
    sink_t *s = &s_sink[sink - 1];
    s->in_ms = in_ms > 60000 ? 60000 : in_ms;
    s->out_ms = out_ms > 60000 ? 60000 : out_ms;
    s->shape_in = shape_in & 3;
    s->shape_out = shape_out & 3;
}

/* the screen's range, -6..+6 steps (stock_amps.md 5): +-12 dB, whatever a head unit with another scale sends - more
   would drive the external amplifier into clipping */
#define TONE_STEPS 6

void dsp_tone(int bass, int treble)
{
    s_bass_to = (int8_t)(bass < -TONE_STEPS ? -TONE_STEPS : bass > TONE_STEPS ? TONE_STEPS : bass);
    s_treble_to = (int8_t)(treble < -TONE_STEPS ? -TONE_STEPS : treble > TONE_STEPS ? TONE_STEPS : treble);
}

/* the mute factor x of the way through a fade: down to 0 when muting, up to 1 when not; degressive changes fast
   first, progressive slow first */
static float fade_at(const sink_t *s, float x)
{
    float k;
    switch (s->fade_shape) {
    case DSP_DEGRESSIVE: k = 1 - (1 - x) * (1 - x); break;
    case DSP_PROGRESSIVE: k = x * x; break;
    default: k = x;
    }
    return s->muted ? s->m_from * (1 - k) : s->m_from + (1 - s->m_from) * k;
}

/* the targets taken at a block's start: a new level starts a glide, a new mute state a fade */
static void take(sink_t *s)
{
    float to[2] = { db2lin(s->to_db[0]), db2lin(s->to_db[1]) };
    if (to[0] != s->g_to[0] || to[1] != s->g_to[1]) {
        s->g_left = s_rate * GLIDE_MS / 1000;
        for (int c = 0; c < 2; c++) {
            s->g_to[c] = to[c];
            s->g_step[c] = (to[c] - s->g[c]) / s->g_left;
        }
    }
    bool mute = s->to_mute;
    if (mute != s->muted) {
        s->muted = mute;
        s->m_from = s->m;
        s->fade_shape = mute ? s->shape_in : s->shape_out;
        s->fade_len = s->fade_shape == DSP_JUMP ? 0 : (int)((mute ? s->in_ms : s->out_ms) * (unsigned)s_rate / 1000);
        s->fade_pos = 0;
        if (!s->fade_len) s->m = mute ? 0 : 1;
    }
}

/* the sink's gains for the next sample, left and right: level times mute, both moving only while a glide or a fade
   runs */
static inline void gain(sink_t *s, float *gl, float *gr)
{
    if (s->g_left) {
        if (--s->g_left) {
            s->g[0] += s->g_step[0];
            s->g[1] += s->g_step[1];
        } else {
            s->g[0] = s->g_to[0];
            s->g[1] = s->g_to[1];
        }
    }
    if (s->fade_len) {
        s->m = ++s->fade_pos < s->fade_len ? fade_at(s, (float)s->fade_pos / s->fade_len) : (s->muted ? 0 : 1);
        if (s->fade_pos >= s->fade_len) s->fade_len = 0;
    }
    *gl = s->g[0] * s->m;
    *gr = s->g[1] * s->m;
}

static inline int32_t out32(float x) { return x >= FULL ? (int32_t)FULL : x <= -FULL ? -(int32_t)FULL : (int32_t)x; }

void dsp_process(const int32_t *music, const int32_t *const mono[3], int32_t *out, int n)
{
    for (int k = 0; k < DSP_SINKS; k++) take(&s_sink[k]);
    /* the tone glides to its targets, a dB per block */
    float bt = s_bass_to * TONE_DB_STEP, tt = s_treble_to * TONE_DB_STEP;
    if (bt != s_bass_db) {
        s_bass_db = fabsf(bt - s_bass_db) <= TONE_GLIDE_DB ? bt : s_bass_db + copysignf(TONE_GLIDE_DB, bt - s_bass_db);
        s_cb = shelf(true, BASS_HZ, s_bass_db);
    }
    if (tt != s_treble_db) {
        s_treble_db = fabsf(tt - s_treble_db) <= TONE_GLIDE_DB ? tt
                                                               : s_treble_db + copysignf(TONE_GLIDE_DB, tt - s_treble_db);
        s_ct = shelf(false, TREBLE_HZ, s_treble_db);
    }
    bool bass_on = s_bass_db != 0, treble_on = s_treble_db != 0;
    /* the balance glides like the tone; it sits on the whole output, the mono sinks included, as in an amplifier */
    float bal = s_bal_to * BAL_DB_STEP;
    if (bal != s_bal_db) {
        s_bal_db = fabsf(bal - s_bal_db) <= TONE_GLIDE_DB ? bal : s_bal_db + copysignf(TONE_GLIDE_DB, bal - s_bal_db);
        s_gl = s_bal_db < 0 ? db2lin(s_bal_db) : 1;
        s_gr = s_bal_db > 0 ? db2lin(-s_bal_db) : 1;
    }
    /* the gate: a new target starts its fade; at 1 it multiplies by exactly 1, so the neutral path stays bit-exact */
    float gto = s_gate_closed ? 0 : 1;
    if (gto != s_gate_to) {
        s_gate_to = gto;
        s_gate_left = (int)((unsigned)s_rate * s_gate_ms / 1000);
        s_gate_step = s_gate_left ? (gto - s_gate) / s_gate_left : 0;
        if (!s_gate_left) s_gate = gto;
    }
    for (int i = 0; i < n; i++) {
        float l = (float)music[2 * i], r = (float)music[2 * i + 1];
        l = biquad(&s_ct, &s_ht[0], biquad(&s_cb, &s_hb[0], l, bass_on), treble_on);
        r = biquad(&s_ct, &s_ht[1], biquad(&s_cb, &s_hb[1], r, bass_on), treble_on);
        float gl, gr;
        gain(&s_sink[0], &gl, &gr);
        l *= gl;
        r *= gr;
        for (int k = 0; k < 3; k++) {
            gain(&s_sink[k + 1], &gl, &gr);
            if (mono[k]) {
                float v = (float)mono[k][i];
                l += v * gl;
                r += v * gr;
            }
        }
        if (s_gate_left) s_gate = --s_gate_left ? s_gate + s_gate_step : s_gate_to;
        out[2 * i] = out32(l * (s_gl * s_gate));
        out[2 * i + 1] = out32(r * (s_gr * s_gate));
    }
}
