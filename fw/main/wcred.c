/* see wcred.h */
#include <string.h>
#include "nvs.h"
#include "wcred.h"

#define NS "wifi"

typedef struct { char ssid[33], pass[65]; } hotspot_t;  /* one blob: a power loss cannot part the name from its password */

static esp_err_t get(const char *key, char *v, size_t n)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e == ESP_OK) {
        size_t len = n;
        e = nvs_get_str(h, key, v, &len);
        nvs_close(h);
    }
    return e == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : e;
}

static esp_err_t put(const char *key, const char *v)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = v ? nvs_set_str(h, key, v) : nvs_erase_key(h, key);
    if (e == ESP_ERR_NVS_NOT_FOUND) e = ESP_OK;         /* forgetting what was never stored */
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t wcred_get(char *ssid, size_t ns, char *pass, size_t np)
{
    nvs_handle_t h;
    hotspot_t v;
    size_t len = sizeof v;
    esp_err_t e = nvs_open(NS, NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    e = nvs_get_blob(h, "hotspot", &v, &len);
    nvs_close(h);
    if (e == ESP_ERR_NVS_NOT_FOUND || (e == ESP_OK && len != sizeof v)) return ESP_ERR_NOT_FOUND;
    if (e != ESP_OK) return e;
    v.ssid[sizeof v.ssid - 1] = v.pass[sizeof v.pass - 1] = 0;
    strlcpy(ssid, v.ssid, ns);
    strlcpy(pass, v.pass, np);
    return ESP_OK;
}

esp_err_t wcred_set(const char *ssid, const char *pass)
{
    hotspot_t v = { 0 };
    strlcpy(v.ssid, ssid, sizeof v.ssid);
    strlcpy(v.pass, pass, sizeof v.pass);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "hotspot", &v, sizeof v);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t wcred_forget(void) { return put("hotspot", NULL); }

void wcred_ap_pass(char *pass, size_t n)
{
    if (get("appass", pass, n) != ESP_OK || strlen(pass) < 8) strlcpy(pass, WCRED_AP_DEFAULT, n);
}

esp_err_t wcred_set_ap_pass(const char *pass)
{
    size_t len = strlen(pass);
    return len < 8 || len > 63 ? ESP_ERR_INVALID_ARG : put("appass", pass);
}
