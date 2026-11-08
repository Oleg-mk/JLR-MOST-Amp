/* see alac_glue.h */
#include <stdint.h>
#include "ALACBitUtilities.h"
#include "ALACDecoder.h"
#include "alac_glue.h"

struct alac {
    ALACDecoder dec;
    uint32_t frame, channels;
};

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

alac_t *alac_new(const uint32_t f[11])
{
    /* ALACSpecificConfig as a magic cookie, big-endian; word-aligned, for Init reads its 32-bit fields in place */
    uint32_t words[6];
    uint8_t *c = (uint8_t *)words;
    be32(c, f[0]);
    c[4] = (uint8_t)f[1];
    c[5] = (uint8_t)f[2];
    c[6] = (uint8_t)f[3];
    c[7] = (uint8_t)f[4];
    c[8] = (uint8_t)f[5];
    c[9] = (uint8_t)f[6];
    c[10] = (uint8_t)(f[7] >> 8);
    c[11] = (uint8_t)f[7];
    be32(c + 12, f[8]);
    be32(c + 16, f[9]);
    be32(c + 20, f[10]);
    if (f[0] < ALAC_FRAME_MIN) return nullptr;
    alac_t *a = new alac_t;
    if (a->dec.Init(c, sizeof words) != 0 || a->dec.mConfig.bitDepth != 16 || a->dec.mConfig.numChannels < 1 ||
        a->dec.mConfig.numChannels > 2) {
        delete a;
        return nullptr;
    }
    a->frame = a->dec.mConfig.frameLength;
    a->channels = a->dec.mConfig.numChannels;
    return a;
}

int alac_decode(alac_t *a, const uint8_t *in, int n, int16_t *out)
{
    BitBuffer b;
    uint32_t got = 0;
    BitBufferInit(&b, (uint8_t *)in, (uint32_t)n);
    if (a->dec.Decode(&b, (uint8_t *)out, a->frame, a->channels, &got) != 0) return -1;
    /* a frame longer than the stream's is refused in the decoder (the "Bitline" lines in ALACDecoder.cpp); one whose
       reading ran past the packet is garbage: silence instead */
    if (got > a->frame || BitBufferGetPosition(&b) > (uint32_t)n * 8) return -1;
    return (int)got;
}

void alac_free(alac_t *a) { delete a; }
