#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const void *device_descriptor;
    const char **string_descriptor;
    int string_descriptor_count;
    bool external_phy;
    bool self_powered;
    int vbus_monitor_io;
} tinyusb_config_t;

esp_err_t tinyusb_driver_install(const tinyusb_config_t *config);
bool tud_connect(void);
bool tud_disconnect(void);
