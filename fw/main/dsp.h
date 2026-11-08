#pragma once
/* Stage 5 sound processing (docs/firmware.md, "Обробка"): what a stock amplifier does with its sinks. Sink 1 - the
   music, stereo, or the phone's stream in its place - goes through Bass and Treble; every sink gets its level from
   SpeakerLevel and its mute with MuteParameters' fade; sinks 2-4 (phone, navigation and voice; parking sensors;
   chimes), mono, go into both channels. Float arithmetic: at neutral settings a 16- or 24-bit sink passes bit for
   bit, since float holds such samples exactly and the tone filters are out of the path at 0. No ESP-IDF here */
#include <stdbool.h>
#include <stdint.h>

#define DSP_SINKS 4

enum { DSP_JUMP, DSP_LINEAR, DSP_DEGRESSIVE, DSP_PROGRESSIVE };     /* MuteParameters' shapes */

void dsp_init(int rate);                                /* all sinks at the default level, unmuted, tone flat */
void dsp_level(int sink, float db);                     /* sink 1..4; -127 dB or less: silence */
/* the same with a level per output: the pre-facelift head unit's balance comes as the levels of the left and the
   right speakers (amp.c), and a mono sink keeps its own left and right too */
void dsp_level_lr(int sink, float db_l, float db_r);
void dsp_mute(int sink, bool on);
void dsp_mute_params(int sink, unsigned in_ms, unsigned out_ms, int shape_in, int shape_out);
void dsp_tone(int bass, int treble);                    /* in the head unit's steps */
/* Balance in the head unit's steps, -10..+10 (AUD3): a negative value moves the sound to the right, the left side
   attenuated by 2 dB a step, a positive one to the left; 0 is bit for bit */
void dsp_balance(int steps);
/* the output gate: closed, the output fades to silence over ms whatever mute times the sinks have, and stays there;
   opened, it comes back the same way. The shutdown sequence (power.c) and an update (webcfg.c) close it */
void dsp_gate(bool open, unsigned ms);
/* n frames. music: sink 1, left and right; mono[0..2]: sinks 2..4, NULL when silent; all 32-bit slots with the sample
   left-justified. out: left, right */
void dsp_process(const int32_t *music, const int32_t *const mono[3], int32_t *out, int n);
int dsp_selftest(void);                                 /* selftest.c: failures, 0 = all passed */
