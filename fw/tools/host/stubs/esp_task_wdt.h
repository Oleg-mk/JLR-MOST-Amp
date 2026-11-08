#pragma once
#include "esp_err.h"
static inline esp_err_t esp_task_wdt_add(void *task) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void *task) { return ESP_OK; }
