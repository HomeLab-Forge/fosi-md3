// Minimal non-connectable BLE beacon used to establish whether the MD3 has a usable antenna.
#pragma once

#include "esp_err.h"

esp_err_t radio_ble_start(void);

/** 0 = starting, 1 = advertising, negative = NimBLE/host error code. */
int radio_ble_status(void);
