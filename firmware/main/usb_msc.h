// TinyUSB mass-storage endpoint for Photo Sync, with no formatting path.
#pragma once

#include <stdint.h>

#include "esp_err.h"

/** Install TinyUSB MSC against the existing wear-levelled storage driver. */
esp_err_t usb_msc_init(void);

/** esp_timer time of the host's most recent TEST UNIT READY; 0 before the first. */
int64_t usb_msc_last_poll_us(void);

/** Drop off the bus for 250 ms and reconnect, so the host enumerates and probes the volume again.
 *  Call from a task, never from a TinyUSB callback. */
void usb_msc_reconnect(void);
