#include "settings.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"

static const char *TAG = "settings";

#define RECORD_OFFSET   0x1000
#define RECORD_SECTOR   0x1000
#define SAVE_DELAY_US   (2 * 1000 * 1000)

md3_settings_t g_settings;

static const esp_partition_t *s_part;
static int64_t s_dirty_since;  // 0 = clean

static void set_defaults(void)
{
    memset(&g_settings, 0, sizeof(g_settings));
    memset(g_settings.pad, 0xFF, sizeof(g_settings.pad));
    g_settings.magic = 0x55;
    g_settings.gain_high = 1;
    g_settings.brightness = 2;
    g_settings.volume = 26;
    g_settings.volume_r = 26;
}

esp_err_t settings_load(void)
{
    set_defaults();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x81, "user_dat");
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "user_dat partition missing");
    md3_settings_t record;
    ESP_RETURN_ON_ERROR(esp_partition_read(s_part, RECORD_OFFSET, &record, sizeof(record)), TAG, "read");
    g_settings = record;

    bool dirty = false;
    if (g_settings.magic != 0x55) {
        ESP_LOGW(TAG, "no valid record (magic %02x); using defaults", g_settings.magic);
        set_defaults();
        dirty = true;
    }
    if (g_settings.volume > 100) {
        g_settings.volume = 26;
        dirty = true;
    }
    if (g_settings.filter > 7) {
        g_settings.filter = 0;
        dirty = true;
    }
    if (g_settings.brightness > 4) {
        g_settings.brightness = 2;
        dirty = true;
    }
    if (g_settings.album_folder >= 6) {
        g_settings.album_folder = 0;
        dirty = true;
    }
    if (g_settings.rotation != (g_settings.rotation & 3)) {
        dirty = true;
    }
    g_settings.rotation &= 3;
    if (g_settings.udisk_mode > 1) {
        g_settings.udisk_mode = 1;
        dirty = true;
    }
    if (dirty) {
        settings_touch();
    }
    ESP_LOGI(TAG, "volume=%u filter=%u gain_high=%u host_vol=%u brightness=%u rotation=%u",
             g_settings.volume, g_settings.filter, g_settings.gain_high, g_settings.vol_from_host,
             g_settings.brightness, g_settings.rotation);
    return ESP_OK;
}

void settings_touch(void)
{
    g_settings.volume_r = g_settings.volume;
    s_dirty_since = esp_timer_get_time();
}

void settings_tick(void)
{
    if (!s_dirty_since || esp_timer_get_time() - s_dirty_since < SAVE_DELAY_US || !s_part) {
        return;
    }
    esp_err_t err = esp_partition_erase_range(s_part, RECORD_SECTOR, s_part->erase_size);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, RECORD_OFFSET, &g_settings, sizeof(g_settings));
    }
    if (err != ESP_OK) {
        // Keep the record dirty and retry after another delay instead of losing the change.
        s_dirty_since = esp_timer_get_time();
        ESP_LOGE(TAG, "save failed (will retry): %s", esp_err_to_name(err));
    } else {
        s_dirty_since = 0;
        ESP_LOGI(TAG, "saved");
    }
}
