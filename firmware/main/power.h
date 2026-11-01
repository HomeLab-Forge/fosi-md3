// VBUS monitor and the "usb src" line, reproducing the stock adc_monitor_task logic.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t power_init(void);

/** Sample once (call every ~50 ms). Returns true when the audio power-up should be rerun. */
bool power_tick(void);

int power_vbus_mv(void);
bool power_charging(void);
