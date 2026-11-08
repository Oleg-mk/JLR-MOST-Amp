#pragma once
/* Sample-rate converter for the phone's sound (docs/firmware.md, "Телефон"): windowed-sinc interpolation over
   SRC_TAPS input frames, the coefficients taken from a cubic per 1/32 of a frame, so any position between two input
   frames is served and the ratio may change from one output frame to the next. The audible band (to 20 kHz) is kept
   flat; images and aliases that would land in it are stopped, those that land above it are let through partly.
   No ESP-IDF here: the selftest runs the same code */
#include <stdint.h>

#define SRC_TAPS    40

/* builds the filter for f_in -> f_out; 0, or -1 for rates it cannot serve */
int src_design(int f_in, int f_out);
/* one output frame at input position i + frac / 2^32. w: the SRC_TAPS input frames i - SRC_TAPS/2 + 1 ..
   i + SRC_TAPS/2, 16-bit left and right; out: left, right with 1.0 = full scale */
void src_frame(const int16_t *w, uint32_t frac, float out[2]);
