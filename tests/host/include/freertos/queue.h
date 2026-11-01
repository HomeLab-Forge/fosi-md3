#pragma once

#include "freertos/FreeRTOS.h"

typedef struct host_queue *QueueHandle_t;

QueueHandle_t xQueueCreate(unsigned int length, unsigned int item_size);
BaseType_t xQueueReceive(QueueHandle_t queue, void *out, BaseType_t wait);
BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *woken);
