/* The firmware's selftests (selftest.c) on the PC, with the real emulation, sound processing and phone path: only the
   OS8104A, the power task and the read mode are not there */
#include <stdio.h>
#include "freertos/task.h"
#include "esp_err.h"
#include "amp.h"
#include "os8104.h"

int amp_selftest(void);
int phone_selftest(void);
int dsp_selftest(void);

esp_err_t os8104_send(const most_msg_t *m, uint8_t *xts) { if (xts) *xts = 0x10; return ESP_OK; }
QueueHandle_t os8104_rx_queue(void) { return NULL; }
void power_on_msg(const most_msg_t *m) {}
bool learn_active(void) { return false; }
void learn_on_msg(const most_msg_t *m) {}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *h) { return pdPASS; }
void vTaskDelay(TickType_t ticks) {}
TickType_t xTaskGetTickCount(void) { return 0; }

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 4096);
    amp_init(&AMP_PRE_HILINE, NULL);
    int fails = amp_selftest();
    fails += phone_selftest();
    fails += dsp_selftest();
    return fails ? 1 : 0;
}
