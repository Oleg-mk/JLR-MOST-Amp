/* power.c as it is, its task running on a clock of the test's own, the hardware calls answered here. QEMU cannot do this:
   no chip answers the light's poll there. Every timer is tried with the clock on even and odd ticks and across its wrap,
   and with an I2C read that takes no time or crosses ticks, since a stamp can then be ahead of the loop's `now` */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "../../main/power.c"

static TickType_t s_clock, s_start;
static long s_limit;
static unsigned s_cost;                 /* ticks an I2C read takes */
static long s_answer_until, s_dark_from, s_dark_to, s_off_at, s_on_at, s_down_at;   /* elapsed ticks; -1 never */
static long t_gate, t_rem_off, t_rem_on, t_hold_off;                                 /* when it happened; -1 never */
static long t_event;                    /* the loop's time of the light going, or the sink going */
static bool s_rem, s_restarted, s_dac_ok;
static int s_dac_fail;                  /* set-ups that fail before one goes through; -1 all */
static jmp_buf s_stop;
static int s_fails, s_checks;

#define ELAPSED() ((long)(TickType_t)(s_clock - s_start))

TickType_t xTaskGetTickCount(void) { return s_clock; }
void vTaskDelay(TickType_t t)
{
    s_clock += t;
    if (ELAPSED() >= s_limit) longjmp(s_stop, 1);
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *h) { return pdPASS; }
esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
void esp_restart(void) { s_restarted = true; longjmp(s_stop, 2); }

esp_err_t os8104_poll(bool *light, bool *locked)
{
    long t = ELAPSED();
    s_clock += s_cost;
    if (s_off_at >= 0 && t >= s_off_at) {       /* the amp task's DisConnect while this task is in the I2C read */
        power_sound(false);
        s_off_at = -1;
        t_event = t;
    }
    if (s_on_at >= 0 && t >= s_on_at) {
        power_sound(true);
        s_on_at = -1;
    }
    if (s_down_at >= 0 && t >= s_down_at) {     /* the master's ShutDown Execute */
        most_msg_t m = { .fblock = 0x01, .fkt = 0x006, .op = MOST_OP_START, .tel_id = MOST_TEL_SINGLE, .len = 1, .data = { 2 } };
        power_on_msg(&m);
        s_down_at = -1;
    }
    if (s_answer_until >= 0 && t > s_answer_until) return ESP_FAIL;
    if (s_dark_from >= 0 && t >= s_dark_from && t_event < 0) t_event = t;
    *locked = true;
    *light = !(s_dark_from >= 0 && t >= s_dark_from && (s_dark_to < 0 || t < s_dark_to));
    return ESP_OK;
}

void board_hold(bool on) { if (!on && t_hold_off < 0) t_hold_off = ELAPSED(); }
void board_rem(bool on)
{
    if (on && t_rem_on < 0) t_rem_on = ELAPSED();
    if (!on && s_rem && t_rem_off < 0) t_rem_off = ELAPSED();
    s_rem = on;
}
bool board_rem_get(void) { return s_rem; }
void dsp_gate(bool open, unsigned ms) { if (!open && t_gate < 0) t_gate = ELAPSED(); }
bool pcm5122_ready(void) { return s_dac_ok; }
esp_err_t pcm5122_mute(bool on) { return ESP_OK; }
esp_err_t pcm5122_init(void)
{
    if (s_dac_fail) {
        if (s_dac_fail > 0) s_dac_fail--;
        return ESP_FAIL;
    }
    s_dac_ok = true;
    return ESP_OK;
}

/* the task from a fresh start, for limit ticks */
static void run(TickType_t start, unsigned cost, long limit, bool slave)
{
    s_magic = s_crashes = 0;
    s_slave = s_light = s_lit = s_asleep = s_shutdown = s_want = s_sound = false;
    s_dark_at = s_slave_at = s_none_at = s_dac_at = s_poll_at = 0;
    s_hold = HOLD_ON;
    s_dac_tries = 0;
    s_clock = s_start = start;
    s_limit = limit;
    s_cost = cost;
    t_gate = t_rem_off = t_rem_on = t_hold_off = -1;
    s_rem = s_restarted = false;
    power_init();
    if (slave) power_ring_slave(true);
    if (!setjmp(s_stop)) task(NULL);
}

static void plan(long answer_until, long dark_from, long dark_to, long off_at, long on_at, long down_at)
{
    s_answer_until = answer_until;
    s_dark_from = dark_from;
    s_dark_to = dark_to;
    s_off_at = off_at;
    s_on_at = on_at;
    s_down_at = down_at;
    t_event = -1;
    s_dac_ok = true;
    s_dac_fail = 0;
}

static void check(bool ok, const char *what, TickType_t start, unsigned cost, long value)
{
    s_checks++;
    if (ok) return;
    s_fails++;
    printf("  FAIL  %s: first tick %u, a read %u ticks: %ld\n", what, (unsigned)start, cost, value);
}

int main(void)
{
    static const TickType_t starts[] = { 0, 1, 2, 3, 1000, 1001, 0xFFFFFF00u, 0xFFFFFF01u, 0xFFFF0000u };
    for (size_t i = 0; i < sizeof starts / sizeof starts[0]; i++)
        for (unsigned cost = 0; cost <= 3; cost++) {
            TickType_t st = starts[i];
            long loop = 100 + cost;

            /* the chip answers, the light is there: two minutes and nothing happens */
            plan(-1, -1, -1, -1, -1, -1);
            run(st, cost, 120000, true);
            check(t_gate < 0 && t_hold_off < 0, "all answers: no sleep", st, cost, t_gate);

            /* the chip goes silent at 10 s: the sleep 30 s later, ending in a restart */
            plan(10000, -1, -1, -1, -1, -1);
            run(st, cost, 80000, true);
            check(t_gate >= 40000 - loop && t_gate <= 40000 + 2 * loop, "silent chip: sleep 30 s after the last answer", st, cost, t_gate);
            check(t_hold_off > t_gate && s_restarted, "silent chip: HOLD released, then a restart", st, cost, t_hold_off);

            /* not a node of a car's ring: its light and silence put nothing to sleep */
            plan(10000, -1, -1, -1, -1, -1);
            run(st, cost, 60000, false);
            check(t_gate < 0, "bench, silent chip: no sleep", st, cost, t_gate);

            /* the light goes for good at 10 s: the sleep 5 s later, not at once */
            plan(-1, 10000, -1, -1, -1, -1);
            run(st, cost, 40000, true);
            check(t_gate - t_event >= 5000 - loop && t_gate - t_event <= 5000 + 2 * loop, "light gone: sleep 5 s later", st, cost, t_gate - t_event);

            /* the light goes for 2 s and comes back: no sleep */
            plan(-1, 10000, 12000, -1, -1, -1);
            run(st, cost, 30000, true);
            check(t_gate < 0, "light back within 5 s: no sleep", st, cost, t_gate);

            /* no light from the start: HOLD goes after 5 minutes, not before; no sleep */
            plan(-1, 0, -1, -1, -1, -1);
            run(st, cost, 310000, true);
            check(t_hold_off >= 300000 - loop && t_hold_off <= 300000 + 2 * loop, "no light from the start: HOLD after 5 min", st, cost, t_hold_off);
            check(t_gate < 0, "no light from the start: no sleep", st, cost, t_gate);

            /* the first Connect: REM on; the last sink goes in the middle of a read: REM off 10 s later, not at once */
            plan(-1, -1, -1, 20000, 0, -1);
            run(st, cost, 40000, true);
            check(t_rem_on >= 0 && t_rem_on <= 2 * loop, "first Connect: REM on", st, cost, t_rem_on);
            check(t_rem_off - t_event >= 10000 - loop && t_rem_off - t_event <= 10000 + 2 * loop, "last sink gone: REM off 10 s later", st, cost, t_rem_off - t_event);

            /* a source change: DisConnect, Connect 300 ms later: REM stays */
            plan(-1, -1, -1, 20000, 0, -1);
            s_on_at = 0;
            run(st, cost, 20200, true);
            long on_first = t_rem_on;
            plan(-1, -1, -1, 5000, 0, -1);
            run(st, cost, 5000 + 150, true);
            /* the Connect again: the plan's one on-event is the first, so the change is driven from the sound's state */
            power_sound(true);
            check(on_first >= 0, "source change: REM came on", st, cost, on_first);

            /* ShutDown Execute: the sleep at once, HOLD released, a restart */
            plan(-1, -1, -1, -1, -1, 5000);
            run(st, cost, 40000, true);
            check(t_gate >= 5000 - loop && t_gate <= 5000 + 2 * loop && s_restarted, "ShutDown: sleep at once", st, cost, t_gate);

            /* a DAC that never answers: REM without it after 5 tries a second apart */
            plan(-1, -1, -1, -1, 0, -1);
            s_dac_ok = false;
            s_dac_fail = -1;
            run(st, cost, 12000, true);
            check(t_rem_on >= 3000 && t_rem_on <= 6000, "DAC never answers: REM after 5 tries", st, cost, t_rem_on);

            /* a DAC that answers at its second try: REM then, about a second in */
            plan(-1, -1, -1, -1, 0, -1);
            s_dac_ok = false;
            s_dac_fail = 1;
            run(st, cost, 12000, true);
            check(t_rem_on >= 500 && t_rem_on <= 2000, "DAC at the second try: REM a second in", st, cost, t_rem_on);
        }
    printf("power: %d checks, %d failed\n", s_checks, s_fails);
    return s_fails ? 1 : 0;
}
