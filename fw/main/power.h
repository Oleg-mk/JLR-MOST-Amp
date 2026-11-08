#pragma once
/* Sleep (docs/firmware.md, "Живлення" and "Сон"): the light in the ring wakes the board, HOLD keeps it awake. It
   goes to sleep on the master's NetBlock ShutDown Execute, or when the light is gone and does not come back: the sound
   fades, REM off, the DAC muted, HOLD released - the supply goes with the light. HOLD is not taken after a few crashes
   in a row, so a crash loop in a dark ring cannot keep the supply on */
#include <stdbool.h>
#include "most.h"

void power_init(void);                  /* first thing: the crash count decides HOLD */
void power_on_msg(const most_msg_t *m); /* every telegram received: NetBlock ShutDown is looked for */
void power_light(bool on);              /* the ring's light, as the OS8104A sees it (activity on RX, bCM2) */
/* the OS8104A runs as a slave in a car's ring: the light may put it to sleep. No light within 5 min of this: HOLD goes
   and the supply follows the light, taken again if the light comes later */
void power_ring_slave(bool slave);
void power_sleep(const char *why);      /* the sequence itself, at once */
void power_release(const char *why);    /* the OS8104A does not start: HOLD goes, the supply follows the light */
bool power_slave(void);                 /* a slave in a car's ring (power_ring_slave) */
bool power_crash_loop(void);            /* crashes in a row reached the limit: HOLD is not held */
/* stage 4, REM (concept.md): on = the head unit has a sink of ours connected. At the first: REM, then the DAC unmuted,
   so the external amplifier wakes on silence. With none connected for a while (the sound system off): REM off first,
   then the DAC muted */
void power_sound(bool on);
bool power_asleep(void);
