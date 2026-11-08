#pragma once
/* Whose music sink 1 plays (docs/firmware.md, "Перемикання джерел"): the head unit's from the ring, or the phone's in
   its place while the head unit's is silent - the head unit on AUX or paused. When the head unit's music comes on, it
   plays and the phone is paused; when the head unit falls silent again, the phone paused so is resumed. Of two phones
   the newer plays (phone.c) */
#include <stdbool.h>
#include <stdint.h>

#define MUSIC_MAX 512               /* frames per block, at most */

typedef struct {
    int hu;                         /* the head unit's music: 0 not known yet, 1 silent, 2 playing */
    float hu_db;                    /* its level, RMS over 0.1 s, dBFS */
    bool phone;                     /* sink 1 plays the phone */
    int paused;                     /* the phone_src_t paused for the head unit, to be resumed; 0 none */
} music_stats_t;

void music_init(void);
/* one block of sink 1, before the sound processing: hu = the ring's sink 1 (32-bit slots, left, right; NULL = no
   sound from the ring), out = what sink 1 plays (may be hu itself); n <= MUSIC_MAX */
void music_block(const int32_t *hu, int32_t *out, int n);
void music_hu_changed(void);    /* the head unit connected, disconnected or (un)muted sink 1, as at a source change */
void music_stats(music_stats_t *st);
