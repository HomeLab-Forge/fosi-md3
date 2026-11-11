#include "audio.h"
#include "board.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "keys.h"
#include "link.h"
#include "logbuf.h"
#include "md3.h"
#include "nvs_flash.h"
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"
#include "usb_msc.h"

static const char *TAG = "main";

static void require_ok(const char *what, esp_err_t result)
{
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(result));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void)
{
    logbuf_init();
    md3_state_init();
    ESP_LOGI(TAG, "%s boot", MD3_FW_VERSION);

    board_gpio_init();
    if (board_vista_held()) {
        ESP_LOGW(TAG, "Vista held: recovery mode; leaving USB-Serial-JTAG available");
        board_usb_mux_set_audio(false);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Never erase NVS automatically: a version/page error must not destroy provisioning.
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (left untouched): %s", esp_err_to_name(result));
    }
    result = settings_load();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "settings unavailable; using defaults without saving: %s", esp_err_to_name(result));
    }
    result = storage_mount();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "photo storage unavailable; volume was not formatted");
    }
    result = usb_msc_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Photo Sync unavailable; storage was not formatted: %s", esp_err_to_name(result));
    }
    result = link_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Savitech link unavailable: %s", esp_err_to_name(result));
    }
    result = power_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "power monitor unavailable: %s", esp_err_to_name(result));
    }
    result = audio_power_cycle();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "DAC not ready; continuing with display and control: %s", esp_err_to_name(result));
    }

    require_ok("backlight init", board_backlight_init());
    board_lcd_t lcd;
    require_ok("LCD init", board_lcd_init(&lcd));
    require_ok("UI init", ui_init(lcd.io, lcd.panel));
    require_ok("UI start", ui_start());
    result = keys_init();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "button init failed: %s", esp_err_to_name(result));
    }

    // The UI/control task now owns event handling, link/power polling and deferred saves.
}
