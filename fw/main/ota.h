#pragma once
/* Updates over Wi-Fi (docs/firmware.md, "Оновлення по Wi-Fi"): a new image comes through the device's page into the
   other app partition and boots on trial; the bootloader takes the previous one back unless the new one confirms */
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct ota ota_t;

ota_t *ota_begin(size_t size);                      /* NULL when there is no room or no partition */
/* the image's bytes in order; the first ones must hold the app description, checked against this project */
esp_err_t ota_write(ota_t *o, const void *b, size_t n);
esp_err_t ota_finish(ota_t *o, bool commit);        /* commit: validate and boot it next; false: drop it */
/* a trial image confirms once the network is up and healthy() holds (asked once a second: main.c), both for 10 s in a
   row; ring_ok false (the OS8104A did not start): it goes back to the previous image at once */
typedef bool (*ota_health_fn)(void);
void ota_confirm_start(bool ring_ok, ota_health_fn healthy);
const char *ota_version(void);                      /* this image's version (git describe at the build) */
void ota_show(void);
