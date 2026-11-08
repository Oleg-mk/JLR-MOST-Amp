/* The ALAC decoder as AirPlay's packets reach it (alac_glue.cpp), its buffers and the output with guard areas: the refused
   stream parameters, frames that must still decode, and a long run of damaged packets - whatever comes, nothing may be
   written outside a buffer. Reads past a packet's end are not seen here; they end in silence (alac_decode).
   test_alac [iterations] */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "alac_glue.h"

enum { GUARD = 512, FILL = 0xA5 };
struct Block { uint8_t *base; size_t size; };
static std::vector<Block> s_blocks;
static int s_fails;

extern "C" void *guard_calloc(size_t n, size_t size)
{
    size_t total = n * size;
    uint8_t *b = (uint8_t *)malloc(total + 2 * GUARD);
    memset(b, FILL, GUARD);
    memset(b + GUARD, 0, total);
    memset(b + GUARD + total, FILL, GUARD);
    s_blocks.push_back({ b, total });
    return b + GUARD;
}

extern "C" void guard_free(void *p)
{
    if (!p) return;
    uint8_t *b = (uint8_t *)p - GUARD;
    for (size_t i = 0; i < s_blocks.size(); i++)
        if (s_blocks[i].base == b) {
            s_blocks.erase(s_blocks.begin() + i);
            break;
        }
    free(b);
}

static int broken(const uint8_t *g, size_t n)
{
    int k = 0;
    for (size_t i = 0; i < n; i++) k += g[i] != FILL;
    return k;
}

static int guards_broken(void)
{
    int k = 0;
    for (const Block &b : s_blocks) k += broken(b.base, GUARD) + broken(b.base + GUARD + b.size, GUARD);
    return k;
}

static uint32_t s_rnd = 0x1BADF00D;
static uint32_t rnd(void)
{
    s_rnd ^= s_rnd << 13;
    s_rnd ^= s_rnd >> 17;
    s_rnd ^= s_rnd << 5;
    return s_rnd;
}

/* MSB first, as the frames are coded */
struct Bits {
    uint8_t *p;
    size_t bit, max;
    void put(uint32_t v, int n)
    {
        for (int i = n - 1; i >= 0; i--) {
            if (bit / 8 >= max) return;
            if ((v >> i) & 1) p[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));
            else p[bit / 8] &= (uint8_t)~(0x80 >> (bit % 8));
            bit++;
        }
    }
};

static alac_t *open_stream(uint32_t frame, uint32_t depth, uint32_t channels, uint32_t pb, uint32_t mb, uint32_t kb)
{
    const uint32_t f[11] = { frame, 0, depth, pb, mb, kb, channels, 255, 0, 0, 44100 };
    return alac_new(f);
}

/* one frame, the output between guard areas; failures counted against what */
static int decode(alac_t *a, const uint8_t *in, int n, uint32_t frame, const char *what)
{
    static std::vector<uint8_t> out;
    size_t size = frame * 4;
    out.assign(size + 2 * GUARD, FILL);
    int got = alac_decode(a, in, n, (int16_t *)(out.data() + GUARD));
    int bad = guards_broken() + broken(out.data(), GUARD) + broken(out.data() + GUARD + size, GUARD);
    if (bad || got > (int)frame) {
        printf("  FAIL  %s: frame %u, %d bytes in: %d guard bytes changed, %d frames decoded\n", what, frame, n, bad, got);
        s_fails++;
        for (Block &b : s_blocks) {     /* the next look starts clean */
            memset(b.base, FILL, GUARD);
            memset(b.base + GUARD + b.size, FILL, GUARD);
        }
    }
    return got;
}

static void expect(bool ok, const char *what)
{
    if (ok) return;
    printf("  FAIL  %s\n", what);
    s_fails++;
}

/* the head of a channel pair (or of one channel) with its predictors, compressed or the escape */
static void head(Bits &w, bool pair, bool partial, uint32_t samples, int shifted, bool escape, int numu, int numv)
{
    w.put(pair ? 1 : 0, 3);             /* ID_CPE, ID_SCE */
    w.put(0, 4);
    w.put(0, 12);
    w.put((partial ? 8u : 0u) | (uint32_t)(shifted << 1) | (escape ? 1u : 0u), 4);
    if (partial) w.put(samples, 32);
    if (escape) return;
    w.put(rnd() & 7, 8);                /* mixBits */
    w.put(rnd() & 0xFF, 8);             /* mixRes */
    for (int ch = 0; ch < (pair ? 2 : 1); ch++) {
        int num = ch ? numv : numu;
        w.put(rnd() & 1 ? 0 : 15, 4);   /* mode */
        w.put(rnd() & 15, 4);           /* denShift */
        w.put(rnd() & 7, 3);            /* pbFactor */
        w.put((uint32_t)num, 5);
        for (int i = 0; i < num; i++) w.put(rnd() & 0xFFFF, 16);
    }
}

static void fixed_cases(void)
{
    /* the stream parameters: a frame too short for the predictor's warm-up, not 16-bit, no channels or too many */
    for (uint32_t f = 0; f < ALAC_FRAME_MIN; f++) expect(!open_stream(f, 16, 2, 40, 10, 14), "a frame under the minimum is refused");
    expect(!open_stream(352, 24, 2, 40, 10, 14), "24-bit is refused");
    expect(!open_stream(352, 16, 0, 40, 10, 14), "no channels are refused");
    expect(!open_stream(352, 16, 3, 40, 10, 14), "3 channels are refused");

    /* the shortest frame taken with the longest warm-up: inside its buffers */
    static uint8_t in[8192];
    alac_t *a = open_stream(ALAC_FRAME_MIN, 16, 2, 40, 10, 14);
    expect(a != nullptr, "a stream of the minimum frame is taken");
    if (a) {
        for (int num : { 1, 4, 8, 16, 30, 31 }) {
            memset(in, 0, sizeof in);
            for (int i = 200; i < 900; i++) in[i] = (uint8_t)rnd();
            Bits w = { in, 0, sizeof in };
            head(w, true, true, 1, 0, false, num, num);
            size_t n = w.bit / 8 + 800;
            decode(a, in, (int)n, ALAC_FRAME_MIN, "the minimum frame, one sample, a long predictor");
        }
        alac_free(a);
    }

    /* a frame longer than the stream's: refused before a sample is written */
    a = open_stream(352, 16, 2, 40, 10, 14);
    expect(a != nullptr, "a stream of 352 is taken");
    if (a) {
        memset(in, 0, sizeof in);
        Bits w = { in, 0, sizeof in };
        head(w, true, true, 353, 0, true, 0, 0);
        for (int i = 0; i < 2 * 353; i++) w.put(0x1234, 16);
        w.put(7, 3);
        expect(decode(a, in, (int)(w.bit / 8 + 1), 352, "353 frames against 352") < 0, "353 frames against 352 are refused");

        /* valid frames still decode, sample for sample: the escape AirPlay senders may use, whole and partial */
        static int16_t pcm[352 * 2], want[352 * 2];
        for (int samples : { 352, 100 }) {
            memset(in, 0, sizeof in);
            Bits v = { in, 0, sizeof in };
            head(v, true, samples != 352, (uint32_t)samples, 0, true, 0, 0);
            for (int i = 0; i < 2 * samples; i++) {
                want[i] = (int16_t)rnd();
                v.put((uint16_t)want[i], 16);
            }
            v.put(7, 3);
            int got = alac_decode(a, in, (int)((v.bit + 7) / 8), pcm);
            expect(got == samples && !memcmp(pcm, want, sizeof *pcm * 2 * samples), "a valid escape frame decodes sample for sample");
        }
        alac_free(a);
    }
    a = open_stream(352, 16, 1, 40, 10, 14);
    if (a) {
        static int16_t pcm[352 * 2], want[352];
        memset(in, 0, sizeof in);
        Bits v = { in, 0, sizeof in };
        head(v, false, false, 352, 0, true, 0, 0);
        for (int i = 0; i < 352; i++) {
            want[i] = (int16_t)rnd();
            v.put((uint16_t)want[i], 16);
        }
        v.put(7, 3);
        int got = alac_decode(a, in, (int)((v.bit + 7) / 8), pcm);
        expect(got == 352 && !memcmp(pcm, want, sizeof want), "a valid mono escape frame decodes sample for sample");
        alac_free(a);
    }
}

static void damaged(long iterations)
{
    static const uint32_t frames[] = { 32, 33, 40, 64, 352, 352, 352, 1024, 4096 };
    static uint8_t in[65536];           /* reads past a packet land here, not on the test's own data */
    long decoded = 0;
    for (long it = 0; it < iterations; it++) {
        uint32_t frame = frames[rnd() % (sizeof frames / sizeof frames[0])], channels = 1 + (rnd() & 1);
        uint32_t pb = rnd() & 0xFF, mb = rnd() & 0xFF, kb = rnd() & 0x1F;
        alac_t *a = open_stream(frame, 16, channels, pb, mb, kb);
        if (!a) continue;
        for (int k = 0; k < 8; k++) {
            int n = 1 + (int)(rnd() % 1536);
            for (int i = 0; i < n + 64; i++) in[i] = (uint8_t)rnd();
            if (rnd() & 3) {            /* most packets start as frames do, then go wrong */
                bool partial = rnd() & 1, pair = rnd() & 3;
                static const uint32_t lengths[] = { 0, 1, 2, 31, 32, 351, 352, 353, 4095, 4096, 4097, 65536, 0x7FFFFFFF, 0xFFFFFFFF };
                uint32_t samples = lengths[rnd() % (sizeof lengths / sizeof lengths[0])];
                Bits w = { in, 0, (size_t)n };
                head(w, pair, partial, samples, (int)(rnd() % 4), !(rnd() & 7), (int)(rnd() % 32), (int)(rnd() % 32));
            }
            if (decode(a, in, n, frame, "a damaged packet") > 0) decoded++;
            if (s_fails > 20) break;
        }
        alac_free(a);
        if (s_fails > 20) break;
    }
    printf("alac: %ld streams of damaged packets, %ld frames came out whole, %d failures\n", iterations, decoded, s_fails);
}

int main(int argc, char **argv)
{
    fixed_cases();
    damaged(argc > 1 ? atol(argv[1]) : 100000);
    return s_fails ? 1 : 0;
}
