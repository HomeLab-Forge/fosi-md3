#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "diskio_wl.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "wear_levelling.h"

static const char *TAG = "storage";

static wl_handle_t s_wl = WL_INVALID_HANDLE;
static bool s_mounted;
static SemaphoreHandle_t s_mutex;
static uint32_t s_holds;         // STORAGE_HOLD_* reasons keeping the volume local
static int64_t s_local_use_us;   // last local mount or file operation
static uint32_t s_mount_gen;     // counts local mounts, so a request check knows which mount it saw

static esp_err_t storage_lock(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_FAIL;
}

static void storage_unlock(void)
{
    xSemaphoreGiveRecursive(s_mutex);
}

esp_err_t storage_mount(void)
{
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (s_mounted) {
        s_local_use_us = esp_timer_get_time();
        storage_unlock();
        return ESP_OK;
    }

    if (s_wl == WL_INVALID_HANDLE) {
        const esp_vfs_fat_mount_config_t cfg = {
            .format_if_mount_failed = false,  // never: the volume holds the user's images
            .max_files = 6,
            .allocation_unit_size = 0,
        };
        result = esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_ROOT, "storage", &cfg, &s_wl);
        if (result != ESP_OK) {
            if (s_wl != WL_INVALID_HANDLE) {
                (void)wl_unmount(s_wl);
                s_wl = WL_INVALID_HANDLE;
            }
            s_mounted = false;
            ESP_LOGE(TAG, "mount failed (%s); volume left untouched", esp_err_to_name(result));
        } else {
            s_mounted = true;
        }
    } else {
        // Reattach the existing wear-levelled driver without unmounting it or formatting.
        const BYTE pdrv = ff_diskio_get_pdrv_wl(s_wl);
        if (pdrv == 0xff) {
            storage_unlock();
            return ESP_ERR_INVALID_STATE;
        }
        char drv[3] = {(char)('0' + pdrv), ':', 0};
        const esp_vfs_fat_conf_t conf = {
            .base_path = STORAGE_ROOT,
            .fat_drive = drv,
            .max_files = 6,
        };
        FATFS *fs = NULL;
        result = esp_vfs_fat_register_cfg(&conf, &fs);
        if (result == ESP_OK) {
            const FRESULT mounted = f_mount(fs, drv, 1);
            if (mounted != FR_OK) {
                (void)esp_vfs_fat_unregister_path(STORAGE_ROOT);
                result = ESP_FAIL;
            } else {
                s_mounted = true;
            }
        }
    }
    if (result == ESP_OK) {
        // Every local mount is checked for host requests before the host may take it back.
        s_holds |= STORAGE_HOLD_REQUESTS;
        s_mount_gen++;
        s_local_use_us = esp_timer_get_time();
        uint64_t total = 0, free_bytes = 0;
        if (storage_usage(&total, &free_bytes) == ESP_OK) {
            ESP_LOGI(TAG, "mounted %s: %llu KB free of %llu KB", STORAGE_ROOT,
                     free_bytes / 1024, total / 1024);
        }
    }
    storage_unlock();
    return result;
}

esp_err_t storage_unmount(void)
{
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        storage_unlock();
        return ESP_OK;
    }
    const BYTE pdrv = ff_diskio_get_pdrv_wl(s_wl);
    if (pdrv == 0xff) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char drv[3] = {(char)('0' + pdrv), ':', 0};
    const FRESULT detached = f_mount(0, drv, 0);
    if (detached != FR_OK) {
        storage_unlock();
        return ESP_FAIL;
    }
    result = esp_vfs_fat_unregister_path(STORAGE_ROOT);
    if (result == ESP_OK) {
        s_mounted = false;
    }
    storage_unlock();
    return result;
}

bool storage_mounted(void)
{
    if (storage_lock() != ESP_OK) {
        return false;
    }
    const bool mounted = s_mounted;
    storage_unlock();
    return mounted;
}

void storage_hold(uint32_t reason, bool on)
{
    if (storage_lock() != ESP_OK) {
        return;
    }
    s_holds = on ? s_holds | reason : s_holds & ~reason;
    storage_unlock();
}

uint32_t storage_mount_generation(void)
{
    if (storage_lock() != ESP_OK) {
        return 0;
    }
    const uint32_t generation = s_mount_gen;
    storage_unlock();
    return generation;
}

void storage_requests_checked(uint32_t generation)
{
    if (storage_lock() != ESP_OK) {
        return;
    }
    if (generation == s_mount_gen) {
        s_holds &= ~STORAGE_HOLD_REQUESTS;
    }
    storage_unlock();
}

uint32_t storage_holds(void)
{
    if (storage_lock() != ESP_OK) {
        return 0;
    }
    const uint32_t holds = s_holds;
    storage_unlock();
    return holds;
}

esp_err_t storage_release_to_host(int64_t idle_us)
{
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        result = ESP_ERR_NOT_FOUND;
    } else if (s_holds || esp_timer_get_time() - s_local_use_us < idle_us) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        result = storage_unmount();
        if (result == ESP_OK) {
            ESP_LOGI(TAG, "volume released to the USB host");
        }
    }
    storage_unlock();
    return result;
}

esp_err_t storage_read(const char *path, uint8_t **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_local_use_us = esp_timer_get_time();
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0) {
        storage_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size > STORAGE_MAX_FILE_BYTES) {
        storage_unlock();
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        storage_unlock();
        return ESP_ERR_NO_MEM;
    }
    FILE *file = fopen(path, "rb");
    if (!file) {
        free(buf);
        storage_unlock();
        return ESP_FAIL;
    }
    const size_t count = fread(buf, 1, st.st_size, file);
    fclose(file);
    if (count != (size_t)st.st_size) {
        free(buf);
        storage_unlock();
        return ESP_FAIL;
    }
    storage_unlock();
    *data = buf;
    *len = count;
    return ESP_OK;
}

esp_err_t storage_write_file(const char *path, const uint8_t *data, size_t len)
{
    if ((!data && len) || len > STORAGE_MAX_FILE_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_local_use_us = esp_timer_get_time();
    FILE *file = fopen(path, "wb");
    if (!file) {
        storage_unlock();
        return ESP_FAIL;
    }
    const size_t written = len ? fwrite(data, 1, len, file) : 0;
    const bool complete = written == len && fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    storage_unlock();
    return complete && closed ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_remove(const char *path)
{
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_local_use_us = esp_timer_get_time();
    result = unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    storage_unlock();
    return result;
}

esp_err_t storage_usage(uint64_t *total, uint64_t *free_bytes)
{
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (!s_mounted) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = esp_vfs_fat_info(STORAGE_ROOT, total, free_bytes);
    storage_unlock();
    return result;
}

uint32_t storage_sector_count(void)
{
    if (storage_lock() != ESP_OK || s_wl == WL_INVALID_HANDLE) {
        if (s_mutex) {
            storage_unlock();
        }
        return 0;
    }
    const size_t sector_size = wl_sector_size(s_wl);
    const uint32_t count = sector_size ? (uint32_t)(wl_size(s_wl) / sector_size) : 0;
    storage_unlock();
    return count;
}

uint32_t storage_sector_size(void)
{
    if (storage_lock() != ESP_OK || s_wl == WL_INVALID_HANDLE) {
        if (s_mutex) {
            storage_unlock();
        }
        return 0;
    }
    const uint32_t size = (uint32_t)wl_sector_size(s_wl);
    storage_unlock();
    return size;
}

static esp_err_t check_sector_range(uint32_t lba, uint32_t offset, size_t size)
{
    const uint32_t sector_size = storage_sector_size();
    const uint32_t sector_count = storage_sector_count();
    if (!sector_size || !sector_count) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!size) {
        return ESP_OK;
    }
    if (size % sector_size) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint64_t start = (uint64_t)lba * sector_size + offset;
    const uint64_t end = start + size;
    if (start > (uint64_t)sector_count * sector_size || end > (uint64_t)sector_count * sector_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t storage_read_sector(uint32_t lba, uint32_t offset, void *buffer, size_t size)
{
    if (!buffer && size) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    result = check_sector_range(lba, offset, size);
    if (result == ESP_OK && size) {
        result = wl_read(s_wl, (size_t)lba * storage_sector_size() + offset, buffer, size);
    }
    storage_unlock();
    return result;
}

esp_err_t storage_write_sector(uint32_t lba, uint32_t offset, const void *buffer, size_t size)
{
    if (!buffer && size) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = storage_lock();
    if (result != ESP_OK) {
        return result;
    }
    if (s_mounted) {
        storage_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    result = check_sector_range(lba, offset, size);
    if (result == ESP_OK && size) {
        const size_t address = (size_t)lba * storage_sector_size() + offset;
        result = wl_erase_range(s_wl, address, size);
        if (result == ESP_OK) {
            result = wl_write(s_wl, address, buffer, size);
        }
    }
    storage_unlock();
    return result;
}
