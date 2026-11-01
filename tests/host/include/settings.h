#pragma once

#include <stdint.h>

typedef struct {
    uint8_t uac1;
    uint8_t udisk_mode;
} md3_settings_t;

extern md3_settings_t g_settings;
