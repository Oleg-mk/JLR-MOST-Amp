#pragma once
#include "FreeRTOS.h"
/* each test gives these its own way: test_power.c runs a clock on them */
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned prio, TaskHandle_t *h);
void vTaskDelay(TickType_t ticks);
TickType_t xTaskGetTickCount(void);
