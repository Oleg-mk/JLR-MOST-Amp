#pragma once
/* C entry to Apple's ALAC decoder (ALACDecoder.cpp) */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alac alac_t;

/* the shortest frame length taken. The predictor's warm-up writes up to 31 samples whatever a frame's length
   (unpc_block, dp_dec.c) into buffers of the frame length; AirPlay's frames are 352 */
#define ALAC_FRAME_MIN 32

/* a decoder for one stream from AirPlay's fmtp numbers: frame length, compatible version, bit depth, pb, mb, kb,
   channels, max run, max frame bytes, average bit rate, sample rate. NULL when they are refused or not 16-bit */
alac_t *alac_new(const uint32_t fmtp[11]);
/* one frame of in[0..n) into out, 16-bit interleaved (room for frame length x channels); frames decoded, or -1 */
int alac_decode(alac_t *a, const uint8_t *in, int n, int16_t *out);
void alac_free(alac_t *a);

#ifdef __cplusplus
}
#endif
