#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef uint32_t TickType_t;

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

void vTaskDelay(TickType_t ticks);
