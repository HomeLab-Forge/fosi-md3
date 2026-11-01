#include "audio.h"

#include "board.h"
#include "dac.h"
#include "esp_log.h"
#include "settings.h"
#include "storage.h"

esp_err_t audio_power_cycle(void)
{
    board_audio_power_begin();
    board_uac_select(g_settings.uac1);
    board_usb_mux_set_audio(!g_settings.udisk_mode);
    if (!g_settings.udisk_mode) {
        const esp_err_t mount_result = storage_mount();
        if (mount_result != ESP_OK) {
            ESP_LOGW("audio", "could not remount photo storage after DAC switch: %s",
                     esp_err_to_name(mount_result));
        }
    }
    const esp_err_t result = dac_init();
    board_audio_power_finish();
    return result;
}
