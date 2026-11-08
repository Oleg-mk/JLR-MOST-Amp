/* see ota.h. Rollback is on in the bootloader (sdkconfig.defaults): a new image boots in the PENDING_VERIFY state; if
   it restarts before esp_ota_mark_app_valid_cancel_rollback(), the bootloader boots the previous image instead. The
   trial ends well only when the setup page and the sound engine started, and the network and the sound task hold for
   STEADY_S in a row - an image that could not be updated again, or plays nothing, is never kept */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "ota.h"
#include "wifi.h"

static const char *TAG = "ota";

#define DESC_AT     (24 + 8)        /* the app description follows the image header and the first segment's header */
#define TRIAL_S     300             /* the network may take the setup access point's 3 minutes to come */
#define STEADY_S    10              /* and must then hold this long, with the page and the sound engine */

static ota_health_fn s_healthy;

struct ota {
    const esp_partition_t *part;
    esp_ota_handle_t h;
    size_t got;
    uint8_t head[DESC_AT + sizeof(esp_app_desc_t)];
};

ota_t *ota_begin(size_t size)
{
    ota_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->part = esp_ota_get_next_update_partition(NULL);
    if (!o->part || size > o->part->size ||
        esp_ota_begin(o->part, OTA_WITH_SEQUENTIAL_WRITES, &o->h) != ESP_OK) {
        free(o);
        return NULL;
    }
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)size, o->part->label);
    return o;
}

esp_err_t ota_write(ota_t *o, const void *b, size_t n)
{
    /* the first bytes are kept until the description can be checked: another project's image is refused before a byte
       of it is written */
    if (o->got < sizeof o->head) {
        size_t k = n < sizeof o->head - o->got ? n : sizeof o->head - o->got;
        memcpy(o->head + o->got, b, k);
        o->got += k;
        b = (const uint8_t *)b + k;
        n -= k;
        if (o->got < sizeof o->head) return ESP_OK;
        const esp_app_desc_t *d = (const esp_app_desc_t *)(o->head + DESC_AT), *me = esp_app_get_description();
        if (o->head[0] != 0xE9 || d->magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strncmp(d->project_name, me->project_name, sizeof d->project_name)) {
            ESP_LOGW(TAG, "not this project's image");
            return ESP_ERR_INVALID_VERSION;
        }
        ESP_LOGI(TAG, "image %.32s, version %.32s", d->project_name, d->version);
        esp_err_t e = esp_ota_write(o->h, o->head, sizeof o->head);
        if (e != ESP_OK) return e;
    }
    o->got += n;
    return n ? esp_ota_write(o->h, b, n) : ESP_OK;
}

esp_err_t ota_finish(ota_t *o, bool commit)
{
    esp_err_t e = ESP_OK;
    if (!commit) {
        esp_ota_abort(o->h);
    } else if ((e = esp_ota_end(o->h)) == ESP_OK) {       /* checksum, hash and chip of the whole image */
        e = esp_ota_set_boot_partition(o->part);
        if (e == ESP_OK) ESP_LOGI(TAG, "%s boots next, on trial", o->part->label);
    }
    free(o);
    return e;
}

static void trial(void *arg)
{
    int steady = 0;
    for (int s = 0; s < TRIAL_S; s++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        wifi_state_t w;
        wifi_state(&w);
        bool ok = (w.link == WIFI_ONLINE || w.setup_ap) && (!s_healthy || s_healthy());
        steady = ok ? steady + 1 : 0;           /* every second of STEADY_S, not one look and a wait */
        if (steady < STEADY_S) continue;
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        if (e == ESP_OK)
            ESP_LOGI(TAG, "version %s confirmed: the page started, the network and the sound task steady for %d s",
                     ota_version(), STEADY_S);
        else
            ESP_LOGE(TAG, "version %s not confirmed (%s): the bootloader may take the previous image back",
                     ota_version(), esp_err_to_name(e));
        vTaskDelete(NULL);
    }
    ESP_LOGE(TAG, "no steady network, page and sound within %d s: back to the previous image", TRIAL_S);
    esp_ota_mark_app_invalid_rollback_and_reboot();
    vTaskDelete(NULL);
}

void ota_confirm_start(bool ring_ok, ota_health_fn healthy)
{
    s_healthy = healthy;
    esp_ota_img_states_t st;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(p, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
    /* an image that cannot start the OS8104A is no amplifier: the previous one at once, not after the trial */
    if (!ring_ok) {
        ESP_LOGE(TAG, "version %s on trial: the OS8104A did not start, back to the previous image", ota_version());
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }
    ESP_LOGI(TAG, "version %s on trial in %s", ota_version(), p->label);
    xTaskCreate(trial, "ota_trial", 3072, NULL, 3, NULL);
}

const char *ota_version(void) { return esp_app_get_description()->version; }

void ota_show(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(p, &st);
    static const char *name[] = { [ESP_OTA_IMG_NEW] = "new", [ESP_OTA_IMG_PENDING_VERIFY] = "on trial",
                                  [ESP_OTA_IMG_VALID] = "confirmed", [ESP_OTA_IMG_INVALID] = "invalid",
                                  [ESP_OTA_IMG_ABORTED] = "aborted" };
    const char *s = st == ESP_OTA_IMG_UNDEFINED ? "as flashed" : st <= ESP_OTA_IMG_ABORTED && name[st] ? name[st] : "?";
    printf("ota: version %s, %s, %s\n", ota_version(), p->label, s);
}
