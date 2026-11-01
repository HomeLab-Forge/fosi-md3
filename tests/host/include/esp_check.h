#pragma once

#include "esp_err.h"

#define ESP_RETURN_ON_ERROR(expr, tag, format, ...) \
    do { \
        const esp_err_t result = (expr); \
        if (result != ESP_OK) { \
            (void)(tag); \
            return result; \
        } \
    } while (0)

#define ESP_RETURN_ON_FALSE(condition, error, tag, format, ...) \
    do { \
        if (!(condition)) { \
            (void)(tag); \
            return (error); \
        } \
    } while (0)
