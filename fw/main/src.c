/* see src.h */
#include <math.h>
#include "src.h"

#define SEG_BITS    5
#define SEGS        (1 << SEG_BITS)     /* cubic pieces per frame of the impulse response */
#define F_PASS      20000.0             /* the audible band, kept flat */

static float s_c[SEGS][SRC_TAPS][4];    /* per piece and tap: c0 + c1 u + c2 u^2 + c3 u^3, u = 0..1 in the piece */

static double bessel_i0(double x)
{
    double s = 1, t = 1;
    for (int k = 1; k < 60 && t > 1e-17 * s; k++) {
        t *= (x / (2 * k)) * (x / (2 * k));
        s += t;
    }
    return s;
}

int src_design(int f_in, int f_out)
{
    /* the design takes ~0.2 s in double arithmetic, which the ESP32 does in software: a new stream at the rate of the
       last one keeps it, so its feeder - AirPlay's packet task, the Bluetooth stack - is not held up */
    static int last_in, last_out;
    if (f_in == last_in && f_out == last_out) return 0;
    if (f_in < 8000 || f_out < 8000) return -1;
    double lo = f_in < f_out ? f_in : f_out;
    double pass = fmin(F_PASS, 0.4535 * lo), pass_out = fmin(F_PASS, 0.4535 * f_out);
    /* aliases and images fold into the band from f_out - pass_out up; going up a lot, the images of the band's top
       land in it unfolded, from f_in - pass up */
    double stop = f_out - pass_out;
    if (f_in - pass < pass_out && f_in - pass < stop) stop = f_in - pass;
    double fc = (pass + stop) / 2 / f_in, dw = 2 * M_PI * (stop - pass) / f_in;      /* per input frame */
    double att = 2.285 * dw * (SRC_TAPS - 1) + 8;       /* Kaiser's estimate for this length, dB */
    double beta = att > 50 ? 0.1102 * (att - 8.7) : att > 21 ? 0.5842 * pow(att - 21, 0.4) + 0.07886 * (att - 21) : 0;
    double i0b = bessel_i0(beta), half = SRC_TAPS / 2.0;
    static double v[4][SRC_TAPS];
    /* the response at u = 0, 1/3, 2/3, 1 of each piece, each set of taps scaled to a sum of 1 so that no position
       changes the gain, then the cubic through the 4 points */
    for (int s = 0; s < SEGS; s++) {
        for (int p = 0; p < 4; p++) {
            double mu = (s + p / 3.0) / SEGS, sum = 0;
            for (int k = 0; k < SRC_TAPS; k++) {
                double t = mu + half - 1 - k, r = t / half, x = 2 * fc * t, q = 1 - r * r;
                double sinc = fabs(x) < 1e-12 ? 1 : sin(M_PI * x) / (M_PI * x);
                sum += v[p][k] = 2 * fc * sinc * bessel_i0(beta * sqrt(q > 0 ? q : 0)) / i0b;
            }
            for (int k = 0; k < SRC_TAPS; k++) v[p][k] /= sum;
        }
        for (int k = 0; k < SRC_TAPS; k++) {
            double a = v[0][k], b = v[1][k], c = v[2][k], d = v[3][k];
            s_c[s][k][0] = (float)a;
            s_c[s][k][1] = (float)((-11 * a + 18 * b - 9 * c + 2 * d) / 2);
            s_c[s][k][2] = (float)((18 * a - 45 * b + 36 * c - 9 * d) / 2);
            s_c[s][k][3] = (float)((-9 * a + 27 * b - 27 * c + 9 * d) / 2);
        }
    }
    last_in = f_in;
    last_out = f_out;
    return 0;
}

void src_frame(const int16_t *w, uint32_t frac, float out[2])
{
    const float (*c)[4] = s_c[frac >> (32 - SEG_BITS)];
    float u = (float)(frac & ((1u << (32 - SEG_BITS)) - 1)) * (1.0f / (1u << (32 - SEG_BITS)));
    float l = 0, r = 0;
    for (int k = 0; k < SRC_TAPS; k++) {
        float h = c[k][0] + u * (c[k][1] + u * (c[k][2] + u * c[k][3]));
        l += h * w[2 * k];
        r += h * w[2 * k + 1];
    }
    out[0] = l * (1.0f / 32768);
    out[1] = r * (1.0f / 32768);
}
