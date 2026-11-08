/* Bitline firmware: board bring-up, bench audio, the ring log, the amplifier emulation and the phone
   (docs/firmware.md) */
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "amp.h"
#include "audio.h"
#include "board.h"
#include "console.h"
#include "dsp.h"
#include "learn.h"
#include "music.h"
#include "os8104.h"
#include "ota.h"
#include "pcm5122.h"
#include "phone.h"
#include "power.h"
#include "raop.h"
#include "webcfg.h"
#include "wifi.h"
#if CONFIG_BT_ENABLED
#include "a2dp.h"
#endif
#if CONFIG_ETH_USE_OPENETH
#include "vphone.h"
#endif

static const char *TAG = "main";

/* stage 0 heartbeat: short blink once a second, so a running board is visible without the UART */
/* D20: a short flash a second while running; reading the stock amplifier a fast blink, read it steady, not read two
   flashes a second; the OS8104A not started three flashes, over all the rest */
static volatile bool s_no_chip;
static volatile bool s_audio_ok, s_web_ok;  /* the sound engine and the page started: an update confirms on them */

/* asked once a second while an update is on trial: the page started, the sound engine started and its task still goes
   round - a block in 4 ms, or at most 400 ms in timeouts with no ring clock */
static bool healthy(void)
{
    bool moving = true;
#if !CONFIG_ETH_USE_OPENETH
    static uint32_t last;
    uint32_t now = audio_frames();
    moving = now != last;
    last = now;
#endif
    return s_audio_ok && s_web_ok && moving;
}

static void flash(int on_ms, int off_ms)
{
    board_led(1);
    vTaskDelay(pdMS_TO_TICKS(on_ms));
    board_led(0);
    vTaskDelay(pdMS_TO_TICKS(off_ms));
}

static void led_task(void *arg)
{
    for (;;) {
        if (s_no_chip) {
            flash(100, 150);
            flash(100, 150);
            flash(100, 650);
            continue;
        }
        switch (learn_state()) {
        case LEARN_RUNNING: flash(125, 125); break;
        case LEARN_DONE:    board_led(1); vTaskDelay(pdMS_TO_TICKS(500)); break;
        case LEARN_FAILED:  flash(100, 150); flash(100, 650); break;
        default:            flash(50, 950); break;
        }
    }
}

static void set_node(uint16_t node) { os8104_set_node(node, 0x00); }

/* into the ring as a slave, tried three times: one I2C error must not cost the drive. A chip that does not start
   lets HOLD go, or the supply would stay on with no ring to end it, and shows three flashes */
static void ring_start(uint16_t node, uint8_t group)
{
    esp_err_t e = ESP_FAIL;
    for (int t = 0; t < 3 && (e = os8104_start_slave(node, group)) != ESP_OK; t++) vTaskDelay(pdMS_TO_TICKS(100));
    if (e == ESP_OK) {
        power_ring_slave(true);
        return;
    }
    s_no_chip = true;
    power_release("the OS8104A does not start");
}

#if !CONFIG_ETH_USE_OPENETH
/* stage 4: what the emulation's sinks and source drive - the OS8104A's routes into the sound engine, REM and the
   DAC, the ring's channel allocation */
static bool dealloc(const uint8_t *ch, int n) { return os8104_dealloc(ch, n) == ESP_OK; }
static int position(void)
{
    uint8_t pos = 0;
    return os8104_read(OS_bNPR, &pos, 1) == ESP_OK ? pos : 0;
}

static const amp_io_t s_io = { audio_sink, power_sound, os8104_alloc, dealloc, os8104_alloc_valid, position };
#endif

void app_main(void)
{
    board_init();
    power_init();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    /* the ESP32 maps 4 MB of the module's 8 MB PSRAM; the rest is reachable only through esp_himem */
    ESP_LOGI(TAG, "Bitline: ESP32 v%d.%d, %d cores, PSRAM %u kB chip, %u kB in heap",
             chip.revision / 100, chip.revision % 100, chip.cores, (unsigned)(esp_psram_get_size() / 1024),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    ESP_LOGI(TAG, "firmware version %s", ota_version());
#ifdef OTA_TEST_CRASH
    /* QEMU's update test only (-DOTA_TEST_CRASH=1): an image that dies before it can confirm, so the bootloader must
       take the previous one back */
    ESP_LOGE(TAG, "version %s: crashing on purpose", ota_version());
    abort();
#endif
    uint8_t s = board_switch();
    ESP_LOGI(TAG, "SW1: type bits %u, read serial %d", (unsigned)(s >> 1), s & 1);
    /* NVS: the hotspot, the Bluetooth pairings, the radio's calibration. The amplifier read from the car lives in a
       partition of its own (partitions.csv, learn.c): the head unit may check its serial number, and the stock
       amplifier may be gone by the time this one is erased */
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS unreadable (%s): erased, the hotspot and the pairings with it", esp_err_to_name(e));
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    ESP_ERROR_CHECK(e);
    e = nvs_flash_init_partition(LEARN_PART);
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "the read amplifier's NVS unreadable (%s): erased, read the stock amplifier again",
                 esp_err_to_name(e));
        if (nvs_flash_erase_partition(LEARN_PART) == ESP_OK) e = nvs_flash_init_partition(LEARN_PART);
    }
    if (e != ESP_OK) ESP_LOGE(TAG, "the read amplifier's NVS: %s - the type's profile only", esp_err_to_name(e));
    /* the OS8104A came out of its own power-on reset unseen: reset it again to catch the power-on interrupt.
       It stays in All-Bypass until the slave start below; 'most master' on the bench takes it over. After a crash
       loop the node stays out of the ring, and the chip is reset only if the last run left it active: the reset
       takes the light off the next node for a moment, and a reboot loop would do that every second */
    if (os8104_init() == ESP_OK) {
        uint8_t xcr = 0;
        if (!power_crash_loop() || os8104_read(OS_bXCR, &xcr, 1) != ESP_OK || (xcr & XCR_ABY)) os8104_reset();
        else ESP_LOGW(TAG, "crash loop: the OS8104A is in All-Bypass already and is left as it is");
    }
    os8104_on_light(power_light);
    /* stage 5: the sinks' levels, mutes and tone, as the emulation passes them on; defaults until the head unit's.
       Sink 1 plays the head unit's music or, while that is silent, the phone's. Set up before the ring starts: the
       head unit's first Connect and levels come right after the lock */
    phone_init();
    dsp_init(PHONE_OUT_RATE);
    music_init();
    /* every control message to the log (stage 2); the node answers as the type SW1 sets or, once read, as the car's
       own amplifier (stage 7) */
    const amp_profile_t *prof = amp_profile_of(s >> 1), *kept = learn_profile(prof), *amp = kept ? kept : prof;
    ESP_LOGI(TAG, "amplifier: %s", amp->name);
    amp_init(amp, amp_tx);
#if !CONFIG_ETH_USE_OPENETH
    amp_set_io(&s_io);              /* QEMU: the ring's sound comes from tools/vamp.py through vphone.c instead */
#endif
    amp_start();
    if (s & 1) {
        /* SW1 "read serial number": a plain node beside the stock amplifier, on the uninitialised address until the
           ring gives this node its position (concept.md, "Серійний номер") */
        os8104_on_ring(learn_ring);
        learn_start(amp_tx, set_node);
        ring_start(0x0FFE, 0x00);
    } else {
#if !CONFIG_ETH_USE_OPENETH
        /* the car: into the ring at once as the stock amplifier, answering - the master asks a new node straight after
           the lock. On the bench 'most master' takes the chip over and 'amp off' silences the emulation. After a crash
           loop the node stays out (All-Bypass): a firmware that keeps falling over must not keep rejoining the car's
           ring */
        if (power_crash_loop()) {
            ESP_LOGE(TAG, "crash loop: the node stays out of the ring, which passes through");
        } else {
            amp_set_active(true);
            ring_start(amp->node, amp->group);
        }
#endif
    }
#if !CONFIG_ETH_USE_OPENETH
    /* the DAC, muted, with the chip's clocks set by the start above and well before the head unit's first Connect, when
       power.c switches REM on and unmutes it. A DAC that failed here is tried again from power.c */
    pcm5122_init();
#endif
#ifdef JLR_SELFTEST
    amp_selftest();
    phone_selftest();
    dsp_selftest();
    dsp_init(PHONE_OUT_RATE);       /* what the tests left in the sound processing goes */
    music_init();
#endif
    /* the phone's hotspot and the setup page; in QEMU the emulated Ethernet */
    wifi_start();
    s_web_ok = webcfg_start();
    raop_start();
    ota_confirm_start(!s_no_chip, healthy);
#if CONFIG_ETH_USE_OPENETH
    /* QEMU: no I2S and no DAC there; the virtual phone stands in for the phone and the ring (tools/vphone.py) */
    vphone_start();
    s_audio_ok = true;
#else
    /* both I2S run on the ring clocks. An engine that did not start leaves a node in the ring with no sound: the log
       says so, and an image on trial is not confirmed with it */
    s_audio_ok = audio_init() == ESP_OK;
    if (!s_audio_ok) ESP_LOGE(TAG, "the sound engine did not start: the node answers the head unit but plays nothing");
#endif
#if CONFIG_BT_ENABLED
    a2dp_start();
#endif
    xTaskCreate(led_task, "led", 2048, NULL, 2, NULL);
    console_start();
}
