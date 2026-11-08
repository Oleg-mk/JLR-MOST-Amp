#pragma once
#include "FreeRTOS.h"
/* nothing queues in the tests: a send is taken, a receive finds nothing */
typedef void *QueueHandle_t;
typedef struct { int unused; } StaticQueue_t;
static inline QueueHandle_t xQueueCreate(unsigned n, unsigned size) { return (QueueHandle_t)1; }
static inline QueueHandle_t xQueueCreateStatic(unsigned n, unsigned size, uint8_t *buf, StaticQueue_t *q) { return (QueueHandle_t)1; }
static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait) { return pdTRUE; }
static inline BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait) { return pdFALSE; }
