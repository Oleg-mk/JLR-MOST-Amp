/* see power.h. The sequence runs in a task of its own: ShutDown arrives in the amplifier's task, the light in the
   OS8104A's, and neither may wait out the fades. HOLD keeps the supply through the whole sequence, whenever the light
   goes. The light is polled over I2C; a chip that stops answering in a car's ring starts the same sequence after
   CHIP_LOST_S, or HOLD could keep the supply on a dark ring for good */
#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "board.h"
#include "dsp.h"
#include "os8104.h"
#include "pcm5122.h"
#include "power.h"

static const char *TAG = "power";

#define CRASHES_MAX     3       /* crashes in a row after which HOLD is not taken */
#define STEADY_S        60      /* up this long: the crash count starts again, and HOLD is taken again */
#define LIGHT_WAIT_MS   5000    /* the light gone this long: sleep - it may come back (MOST 2.5, tPwrSwitchOffDelay) */
#define FIRST_LIGHT_S   300     /* a slave start that sees no light this long: there is no ring to wait for. Long enough
                                   for the bench (SW2, then 'most master' or an update over Wi-Fi); in a car, minutes
                                   of supply after a wake-up that never brought the ring up */
#define FADE_MS         300     /* the output fades over this, whatever mute times the head unit set, then REM */
#define REM_OFF_MS      300     /* the external amplifier switches off before the DAC goes quiet (concept.md, REM) */
#define RESTART_S       20      /* still alive this long after HOLD went: the light stayed, start afresh */
#define REM_ON_MS       300     /* REM before the DAC unmutes: the external amplifier wakes on silence (concept.md) */
#define NO_SINK_S       10      /* no sink connected this long: the sound system is off; a source change is far
                                   shorter (DisConnect, then Connect) */
#define DAC_RETRY_MS    1000    /* a DAC that did not set up is tried again this often ... */
#define DAC_TRIES       5       /* ... and after this many failures REM comes on without it: S/PDIF and HDMI need it */
#define CHIP_LOST_S     30      /* the OS8104A not answering this long in a car's ring: the light cannot be seen any
                                   more, so the sleep sequence runs - HOLD must not keep the supply on a dark ring */
#define NB_FBLOCK       0x01
#define F_SHUTDOWN      0x006   /* NetBlock ShutDown, Start [Control]: 00h Query, 02h Execute (NetBlock 2.5.1) */
#define MAGIC           0x4A4C5221u

enum { HOLD_ON, HOLD_CRASHES, HOLD_NO_LIGHT, HOLD_NO_CHIP };   /* why HOLD was let go */

RTC_NOINIT_ATTR static uint32_t s_magic, s_crashes;     /* kept through a software reset */
static volatile bool s_slave, s_light, s_lit, s_asleep, s_shutdown;    /* s_lit: light seen since the start */
static volatile TickType_t s_dark_at;   /* when the light went; 0 while it is there */
static volatile TickType_t s_slave_at;  /* when the slave start came with no light yet; 0 once light was seen */
static volatile bool s_want;            /* the head unit has a sink of ours connected */
static volatile TickType_t s_none_at;   /* when the last sink went; 0 while one is connected */
static volatile int s_hold;             /* HOLD_ON, or why HOLD was let go */
static bool s_sound;                    /* REM on and the DAC unmuted (or given up on); only the power task changes it */
static int s_dac_tries;
static TickType_t s_dac_at;
static TickType_t s_poll_at;            /* when the OS8104A last answered the light poll; 0 outside a car's ring */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* how long ago a stamp was taken. A stamp can be ahead of the `now` of the loop reading it: the | 1 that keeps it from
   the "not set" value of 0 rounds an even tick up, and the poll or another task may set it after the loop read the
   clock. An unsigned difference would wrap to years there and every timer would fire at once, so it is taken as signed:
   such a stamp is 0 ticks old. The stamps here live seconds to minutes, so the clock's wrap at 49 days does no harm */
static inline TickType_t ago(TickType_t now, TickType_t at)
{
    int32_t d = (int32_t)(now - at);
    return d > 0 ? (TickType_t)d : 0;
}

static void let_go(int why, const char *msg)
{
    if (s_hold != HOLD_ON) return;
    s_hold = why;
    board_hold(false);
    ESP_LOGE(TAG, "HOLD released: %s; the supply now follows the ring's light", msg);
}

void power_sleep(const char *why)
{
    taskENTER_CRITICAL(&s_mux);
    bool was = s_asleep;
    s_asleep = true;
    taskEXIT_CRITICAL(&s_mux);
    if (was) return;
    esp_task_wdt_delete(NULL);      /* the waits below outlast the watchdog; from a task it does not watch, a no-op */
    ESP_LOGI(TAG, "sleep: %s", why);
    dsp_gate(false, FADE_MS);       /* the output gate, not the sinks' mutes: MuteParameters may ask for up to 60 s */
    vTaskDelay(pdMS_TO_TICKS(FADE_MS));
    board_rem(false);
    ESP_LOGI(TAG, "sound faded, REM off");
    vTaskDelay(pdMS_TO_TICKS(REM_OFF_MS));
    pcm5122_mute(true);
    board_hold(false);
    ESP_LOGI(TAG, "DAC muted, HOLD released: the supply goes with the light");
    vTaskDelay(pdMS_TO_TICKS(RESTART_S * 1000));
    ESP_LOGW(TAG, "the light stayed %d s: starting afresh", RESTART_S);
    esp_restart();
}

/* the head unit connected sound (concept.md, REM): the DAC set up and muted, REM, then the DAC unmuted. A DAC that
   failed - its bus or its registers - is tried again every second, from the start, and after DAC_TRIES failures REM
   comes on without it. main.c sets the DAC up right after the ring's start, well before the first Connect */
static void sound_on(TickType_t now)
{
    if (!pcm5122_ready() && s_dac_tries < DAC_TRIES) {
        if (s_dac_at && ago(now, s_dac_at) < pdMS_TO_TICKS(DAC_RETRY_MS)) return;
        s_dac_at = now | 1;
        if (pcm5122_init() != ESP_OK && ++s_dac_tries == DAC_TRIES)
            ESP_LOGE(TAG, "the DAC does not answer: REM on without it");
        if (!pcm5122_ready() && s_dac_tries < DAC_TRIES) return;
    }
    if (!board_rem_get()) {
        board_rem(true);
        ESP_LOGI(TAG, "the head unit connected sound: REM on");
        vTaskDelay(pdMS_TO_TICKS(REM_ON_MS));
    }
    if (!pcm5122_ready()) s_sound = true;
    else if (pcm5122_mute(false) == ESP_OK) {
        ESP_LOGI(TAG, "DAC unmuted");
        s_sound = true;
    }
}

/* no sink for NO_SINK_S: REM off first, then the DAC (concept.md, REM) */
static void sound_off(void)
{
    board_rem(false);
    ESP_LOGI(TAG, "no sink connected for %d s: REM off", NO_SINK_S);
    vTaskDelay(pdMS_TO_TICKS(REM_OFF_MS));
    pcm5122_mute(true);
    ESP_LOGI(TAG, "DAC muted");
    s_sound = false;
}

static void task(void *arg)
{
    TickType_t up = xTaskGetTickCount();
    esp_task_wdt_add(NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_task_wdt_reset();
        TickType_t now = xTaskGetTickCount();
        if (s_crashes && now - up > pdMS_TO_TICKS(STEADY_S * 1000)) s_crashes = 0;
        /* HOLD again once steady, but only for a node that follows the ring's light: after a crash loop the auto-start
           leaves the node out of the ring, and with HOLD on nothing would ever let the supply go */
        if (!s_crashes && s_hold == HOLD_CRASHES && s_slave) {
            s_hold = HOLD_ON;
            board_hold(true);
            ESP_LOGI(TAG, "steady, a node in the ring: HOLD taken again");
        }
        /* the light from bCM2 itself every 100 ms: the INT events alone see it only on edges - ALC comes with a change
           of the ring's size or delay, not with every relock (DS8104AFP4 6.2.8), and a lock lost while RX is still
           active leaves no later event at the dark */
        bool light, locked;
        if (os8104_poll(&light, &locked) == ESP_OK) {
            s_poll_at = now | 1;
            if (s_slave) power_light(light);
        }
        TickType_t dark = s_dark_at, slave_at = s_slave_at, none = s_none_at, polled = s_poll_at;  /* each read once */
        if (s_shutdown) power_sleep("ShutDown Execute from the master");
        else if (s_slave && dark && ago(now, dark) >= pdMS_TO_TICKS(LIGHT_WAIT_MS))
            power_sleep("the light went and did not come back");
        /* no answer over I2C: the light's state is unknown, and a dark ring must not find HOLD on. The sequence ends
           in a restart after RESTART_S, which resets the chip; if it still does not start, the auto-start lets HOLD go
           and the supply follows the light */
        else if (s_slave && polled && ago(now, polled) >= pdMS_TO_TICKS(CHIP_LOST_S * 1000))
            power_sleep("the OS8104A has not answered for 30 s");
        else if (s_slave && slave_at && ago(now, slave_at) >= pdMS_TO_TICKS(FIRST_LIGHT_S * 1000)) {
            s_slave_at = 0;
            let_go(HOLD_NO_LIGHT, "no light in the ring since the start");
        } else if (s_want && !s_sound && !s_asleep) sound_on(now);
        else if (!s_want && (s_sound || board_rem_get()) && none && ago(now, none) >= pdMS_TO_TICKS(NO_SINK_S * 1000))
            sound_off();
    }
}

void power_sound(bool on)
{
    s_none_at = on ? 0 : xTaskGetTickCount() | 1;
    s_want = on;
}

void power_init(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (s_magic != MAGIC || !crash) {
        s_magic = MAGIC;
        s_crashes = 0;
    } else {
        s_crashes++;
    }
    if (s_crashes >= CRASHES_MAX) {
        char why[40];
        snprintf(why, sizeof why, "%u crashes in a row", (unsigned)s_crashes);
        let_go(HOLD_CRASHES, why);
    }
    xTaskCreate(task, "power", 3072, NULL, 3, NULL);
}

/* ShutDown Execute is irrevocable and gets no answer; a node with nothing more to say leaves Query unanswered too
   (MOST 2.5, 3.2.5) */
void power_on_msg(const most_msg_t *m)
{
    if (m->fblock == NB_FBLOCK && m->fkt == F_SHUTDOWN && m->op == MOST_OP_START && m->tel_id == MOST_TEL_SINGLE &&
        m->len >= 1 && m->data[0] == 0x02)
        s_shutdown = true;
}

void power_light(bool on)
{
    if (on) {
        s_light = true;
        s_dark_at = 0;
        s_slave_at = 0;
        if (!s_lit) {
            s_lit = true;
            if (s_hold == HOLD_NO_LIGHT && s_crashes < CRASHES_MAX) {   /* the ring came after all */
                s_hold = HOLD_ON;
                board_hold(true);
                ESP_LOGI(TAG, "light in the ring: HOLD taken again");
            }
        }
    } else if (s_light) {
        s_light = false;
        s_dark_at = xTaskGetTickCount() | 1;
    }
}

void power_ring_slave(bool slave)
{
    s_slave_at = slave && !s_light ? xTaskGetTickCount() | 1 : 0;
    if (!slave) s_poll_at = 0;      /* the no-answer time counts from the first poll the chip answers in the ring: in
                                       QEMU the virtual ring has no chip, and none ever does */
    s_slave = slave;
}

void power_release(const char *why) { let_go(HOLD_NO_CHIP, why); }
bool power_slave(void) { return s_slave; }
bool power_crash_loop(void) { return s_crashes >= CRASHES_MAX; }
bool power_asleep(void) { return s_asleep; }
