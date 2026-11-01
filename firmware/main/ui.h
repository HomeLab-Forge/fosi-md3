// LVGL user interface: main dial, album (Photo/Game folders), remote content pushed over
// the network, and the settings menu. All screen changes run on the UI task.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);

/** Start the UI task that consumes md3_post() events. */
esp_err_t ui_start(void);

/** Name of the current screen: "main", "album", "remote", "menu". */
const char *ui_screen_name(void);

/** Capture the screen as a 24-bit BMP in PSRAM. Caller frees. */
esp_err_t ui_screenshot_bmp(uint8_t **out, size_t *len);
