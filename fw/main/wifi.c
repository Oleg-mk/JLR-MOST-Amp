/* see wifi.h. A station on the phone's hotspot, 2.4 GHz only: an iPhone's hotspot needs "Maximize Compatibility" on.
   The setup access point comes up when no hotspot is stored, when the stored one has not been joined within 3 minutes
   of the start or of a change on the page, or on 'wifi setup'; it goes down once the hotspot is joined (unless it was
   switched on by hand), and one that came up beside a stored hotspot goes down again after 10 minutes with nobody on
   it: a drive without the phone's hotspot must not leave the point open all the way. With Bluetooth on, ESP-IDF's
   coexistence needs modem sleep, which is the default */
#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wcred.h"
#include "wifi.h"

static const char *TAG = "wifi";

#define SETUP_AFTER_US  (180 * 1000000LL)
#define SETUP_DOWN_US   (600 * 1000000LL)   /* a setup point beside a stored hotspot, with nobody on it */

static const int s_wait_s[] = { 1, 2, 5, 10, 30 };     /* between attempts to join */

static esp_netif_t *s_sta;
static esp_timer_handle_t s_retry, s_late, s_down;
static volatile wifi_link_t s_link;
static volatile uint8_t s_reason;
static char s_ssid[33], s_ip[16];
static bool s_have, s_setup, s_by_hand;
static int s_tries, s_on_ap;        /* phones on the setup access point */

static void setup_ap(bool on)
{
    if (on == s_setup) return;
    if (on) {
        wifi_config_t ap = { .ap = { .ssid = WIFI_AP_SSID, .ssid_len = strlen(WIFI_AP_SSID), .max_connection = 2,
                                     .authmode = WIFI_AUTH_WPA2_PSK } };
        wcred_ap_pass((char *)ap.ap.password, sizeof ap.ap.password);
        esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &ap);
        if (e != ESP_OK) {      /* no flag for a point that is not there: an update on trial must not confirm on it */
            ESP_LOGE(TAG, "setup access point not up: %s", esp_err_to_name(e));
            return;
        }
        s_setup = true;
        if (s_have) esp_timer_start_once(s_down, SETUP_DOWN_US);
        ESP_LOGI(TAG, "setup access point '%s' up, page at http://%s", WIFI_AP_SSID, WIFI_AP_IP);
    } else {
        s_setup = false;
        esp_timer_stop(s_down);
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_by_hand = false;
        s_on_ap = 0;
        ESP_LOGI(TAG, "setup access point down");
    }
}

/* SETUP_DOWN_US on: down unless switched on by hand, no hotspot stored, or a phone is on it (then as long again) */
static void down(void *arg)
{
    if (!s_setup || s_by_hand || !s_have) return;
    if (s_on_ap) {
        esp_timer_start_once(s_down, SETUP_DOWN_US);
        return;
    }
    ESP_LOGI(TAG, "nobody came to the setup access point in 10 min; it comes again at the next start");
    setup_ap(false);
}

/* the stored hotspot into the station's configuration; false when none is stored */
static bool load(void)
{
    char pass[65] = "";
    s_have = wcred_get(s_ssid, sizeof s_ssid, pass, sizeof pass) == ESP_OK && s_ssid[0];
    if (!s_have) {
        s_ssid[0] = 0;
        return false;
    }
    wifi_config_t sc = { 0 };
    strlcpy((char *)sc.sta.ssid, s_ssid, sizeof sc.sta.ssid);
    strlcpy((char *)sc.sta.password, pass, sizeof sc.sta.password);
    sc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    sc.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sc.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &sc);
    return true;
}

static void join(void)
{
    s_link = WIFI_JOINING;
    esp_wifi_connect();
}

static void retry(void *arg) { if (s_have && s_link != WIFI_ONLINE) join(); }
static void late(void *arg) { if (s_link != WIFI_ONLINE) setup_ap(true); }

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have) join();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        uint8_t r = ((wifi_event_sta_disconnected_t *)data)->reason;
        s_ip[0] = 0;
        if (r == WIFI_REASON_ASSOC_LEAVE) return;      /* we left it ourselves: wifi_apply or a scan joins again */
        s_link = s_have ? WIFI_JOINING : WIFI_NONE;
        s_reason = r;
        int w = s_wait_s[s_tries < 4 ? s_tries++ : 4];
        ESP_LOGW(TAG, "'%s' not joined (reason %u), again in %d s", s_ssid, r, w);
        if (s_have) esp_timer_start_once(s_retry, w * 1000000LL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        esp_ip4addr_ntoa(&((ip_event_got_ip_t *)data)->ip_info.ip, s_ip, sizeof s_ip);
        s_link = WIFI_ONLINE;
        s_reason = 0;
        s_tries = 0;
        ESP_LOGI(TAG, "on '%s' as %s", s_ssid, s_ip);
        if (s_setup && !s_by_hand) setup_ap(false);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_on_ap++;
        ESP_LOGI(TAG, "a phone joined the setup access point");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_on_ap) s_on_ap--;
    }
}

/* a failure here (no memory, say) leaves the device without Wi-Fi, not rebooting: it is in the car's ring by now, and
   a reboot loop would take the node out and in again every second (the sound and the phone over Bluetooth stay) */
#define TRY(x) do { esp_err_t e_ = (x); \
                    if (e_ != ESP_OK) { ESP_LOGE(TAG, "%s: %s - no Wi-Fi", #x, esp_err_to_name(e_)); return; } } while (0)

void wifi_start(void)
{
    TRY(esp_netif_init());
    TRY(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "bitline");
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&ic));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);     /* the hotspot lives in our own NVS namespace (wcred.c) */
    TRY(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    TRY(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    const esp_timer_create_args_t ra = { .callback = retry, .name = "wifi_retry" }, la = { .callback = late,
                                                                                           .name = "wifi_setup" };
    const esp_timer_create_args_t da = { .callback = down, .name = "wifi_setup_down" };
    TRY(esp_timer_create(&ra, &s_retry));
    TRY(esp_timer_create(&la, &s_late));
    TRY(esp_timer_create(&da, &s_down));
    TRY(esp_wifi_set_mode(WIFI_MODE_STA));
    if (load()) {
        ESP_LOGI(TAG, "joining '%s'", s_ssid);
        esp_timer_start_once(s_late, SETUP_AFTER_US);
    } else {
        ESP_LOGI(TAG, "no hotspot stored");
        setup_ap(true);
    }
    TRY(esp_wifi_start());
}

void wifi_state(wifi_state_t *st)
{
    memset(st, 0, sizeof *st);
    st->link = s_link;
    st->reason = s_reason;
    st->setup_ap = s_setup;
    strlcpy(st->ssid, s_ssid, sizeof st->ssid);
    strlcpy(st->ip, s_ip, sizeof st->ip);
    wifi_ap_record_t ap;
    if (s_link == WIFI_ONLINE && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) st->rssi = ap.rssi;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_record_t *)b)->rssi - ((const wifi_ap_record_t *)a)->rssi;
}

int wifi_scan(wifi_seen_t *out, int max)
{
    /* the driver refuses to scan while the station is joining: stop that for the scan */
    bool joining = s_link == WIFI_JOINING;
    if (joining) {
        esp_timer_stop(s_retry);
        esp_wifi_disconnect();
    }
    uint16_t n = 0;
    wifi_ap_record_t *r = NULL;
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) == ESP_OK && esp_wifi_scan_get_ap_num(&n) == ESP_OK && n) {
        if (n > 24) n = 24;
        r = calloc(n, sizeof *r);
        if (!r || esp_wifi_scan_get_ap_records(&n, r) != ESP_OK) n = 0;
    }
    if (n) qsort(r, n, sizeof *r, by_rssi);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!r[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k && !dup; j++) dup = !strcmp(out[j].ssid, (const char *)r[i].ssid);
        if (dup) continue;
        strlcpy(out[k].ssid, (const char *)r[i].ssid, sizeof out[k].ssid);
        out[k].rssi = r[i].rssi;
        out[k].open = r[i].authmode == WIFI_AUTH_OPEN;
        k++;
    }
    free(r);
    if (joining) join();
    return k;
}

void wifi_apply(void)
{
    esp_timer_stop(s_retry);
    esp_wifi_disconnect();
    s_tries = 0;
    s_reason = 0;
    if (!load()) {
        s_link = WIFI_NONE;
        setup_ap(true);
        return;
    }
    ESP_LOGI(TAG, "joining '%s'", s_ssid);
    /* a hotspot saved wrong on the page, the phone on the point meanwhile: the point comes back 3 min on, as at the
       start, so the page can be reached to put it right */
    esp_timer_stop(s_late);
    esp_timer_start_once(s_late, SETUP_AFTER_US);
    join();
}

void wifi_setup(bool on)
{
    setup_ap(on);
    s_by_hand = on;
}
