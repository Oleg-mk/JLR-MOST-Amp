#pragma once
/* Wi-Fi settings in NVS: the phone's hotspot (name, password) and the setup access point's password */
#include <stddef.h>
#include "esp_err.h"

#define WCRED_AP_DEFAULT "bitlinesetup"   /* the setup access point's password until one is set on the page */

esp_err_t wcred_get(char *ssid, size_t ns, char *pass, size_t np);     /* ESP_ERR_NOT_FOUND when none is stored */
esp_err_t wcred_set(const char *ssid, const char *pass);
esp_err_t wcred_forget(void);
void wcred_ap_pass(char *pass, size_t n);                               /* the stored one, or the default */
esp_err_t wcred_set_ap_pass(const char *pass);                          /* 8..63 characters */
