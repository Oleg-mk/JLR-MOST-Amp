#pragma once
#include "FreeRTOS.h"
/* one thread: every lock is free */
typedef void *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) { return pdTRUE; }
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return pdTRUE; }
