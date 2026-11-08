#pragma once
/* The phone's sound on the ring's clock (docs/firmware.md, "Телефон"). A2DP, AirPlay or, in QEMU, the virtual phone
   feeds 16-bit stereo at its own rate and pace; the sound engine pulls 48 kHz frames at the ring's pace. Between them
   a FIFO and the sample-rate converter, whose ratio follows the FIFO's fill: the two clocks never agree exactly and
   the phone's sound comes in bursts. One source plays at a time: the latest to start, and the one it replaces is
   paused (docs/firmware.md, "Перемикання джерел") */
#include <stdbool.h>
#include <stdint.h>

#define PHONE_OUT_RATE  48000   /* JLR rings run at 48 kHz (stock_amps.md 4) */

typedef enum { PHONE_NONE, PHONE_A2DP, PHONE_AIRPLAY, PHONE_VIRTUAL } phone_src_t;
typedef enum { PHONE_PAUSE, PHONE_PLAY } phone_cmd_t;
typedef void (*phone_remote_fn)(phone_cmd_t cmd);

typedef struct {
    phone_src_t src;            /* who plays */
    int rate;                   /* its sample rate, 0 = no stream */
    bool playing;               /* past the prefill */
    float fill_ms;              /* FIFO fill, smoothed while playing */
    float corr_ppm;             /* the ratio's correction: + when the phone's clock is the faster */
    uint32_t in_frames, out_frames, underruns, overruns;
} phone_stats_t;

void phone_init(void);
void phone_start(phone_src_t src, int rate);        /* a new stream, taking over from whatever played */
void phone_stop(phone_src_t src);                   /* ends the stream if src still owns it */
bool phone_active(void);
phone_src_t phone_owner(uint32_t *gen);             /* who streams (PHONE_NONE: no stream) and that stream's number */
/* pausing and resuming a source as its own buttons do: each source gives its way, and phone_remote() asks from any
   task, the sound engine's too - the call itself is made from the phone's control task */
void phone_remote_set(phone_src_t src, phone_remote_fn fn);
void phone_remote(phone_src_t src, phone_cmd_t cmd);
void phone_feed(phone_src_t src, const int16_t *lr, int frames);   /* ignored unless src owns the stream; what does
                                                                      not fit is dropped */
void phone_gain(float db);                          /* the phone's own volume (AirPlay): 0 dB full, -144 or less mute */
/* n frames at 48 kHz in 32-bit slots, left and right; silence while prefilling or without a stream. out = NULL: the
   positions and the rate control advance without computing sound (the selftest's long runs) */
void phone_pull(int32_t *out, int n);
void phone_stats(phone_stats_t *st);
int phone_selftest(void);                           /* selftest.c: failures, 0 = all passed */
