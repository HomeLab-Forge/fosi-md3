#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "storage.c"

#define TEST_CAPACITY 0x15D0000

static FATFS fake_fs;
static int initial_mount_calls;
static int register_calls;
static int unregister_calls;
static int detach_calls;
static int format_calls;
static int take_calls;
static int give_calls;
static esp_err_t mount_result;
static size_t last_read_address;
static size_t last_erase_address;
static size_t last_write_address;
static wl_handle_t last_unmount_handle;

static void reset_fakes(void)
{
    s_wl = WL_INVALID_HANDLE;
    s_mounted = false;
    s_mutex = NULL;
    initial_mount_calls = register_calls = unregister_calls = detach_calls = format_calls = 0;
    take_calls = give_calls = 0;
    mount_result = ESP_OK;
    last_read_address = last_erase_address = last_write_address = 0;
    last_unmount_handle = WL_INVALID_HANDLE;
}

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void)
{
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t mutex, BaseType_t wait)
{
    (void)mutex;
    (void)wait;
    take_calls++;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t mutex)
{
    (void)mutex;
    give_calls++;
    return pdTRUE;
}

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(
    const char *base_path, const char *partition_label,
    const esp_vfs_fat_mount_config_t *mount_config, wl_handle_t *wl_handle)
{
    (void)base_path;
    (void)partition_label;
    (void)mount_config;
    initial_mount_calls++;
    *wl_handle = 1;
    return mount_result;
}

esp_err_t esp_vfs_fat_unregister_path(const char *base_path)
{
    (void)base_path;
    unregister_calls++;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *conf, FATFS **out_fs)
{
    (void)conf;
    register_calls++;
    *out_fs = &fake_fs;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes)
{
    (void)base_path;
    *total = TEST_CAPACITY;
    *free_bytes = TEST_CAPACITY / 2;
    return ESP_OK;
}

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt)
{
    (void)fs;
    (void)path;
    if (opt == 0) {
        detach_calls++;
    }
    return FR_OK;
}

FRESULT f_mkfs(const char *path, const void *opt, void *work, unsigned int len)
{
    (void)path;
    (void)opt;
    (void)work;
    (void)len;
    format_calls++;
    return FR_FAIL;
}

unsigned char ff_diskio_get_pdrv_wl(wl_handle_t flash_handle)
{
    (void)flash_handle;
    return 0;
}

void ff_diskio_clear_pdrv_wl(wl_handle_t flash_handle)
{
    (void)flash_handle;
}

esp_err_t ff_diskio_register_wl_partition(unsigned char pdrv, wl_handle_t flash_handle)
{
    (void)pdrv;
    (void)flash_handle;
    return ESP_OK;
}

size_t wl_size(wl_handle_t handle)
{
    (void)handle;
    return TEST_CAPACITY;
}

size_t wl_sector_size(wl_handle_t handle)
{
    (void)handle;
    return 4096;
}

esp_err_t wl_read(wl_handle_t handle, size_t src_addr, void *dest, size_t size)
{
    (void)handle;
    (void)dest;
    (void)size;
    last_read_address = src_addr;
    return ESP_OK;
}

esp_err_t wl_write(wl_handle_t handle, size_t dest_addr, const void *src, size_t size)
{
    (void)handle;
    (void)src;
    (void)size;
    last_write_address = dest_addr;
    return ESP_OK;
}

esp_err_t wl_erase_range(wl_handle_t handle, size_t start_addr, size_t size)
{
    (void)handle;
    (void)size;
    last_erase_address = start_addr;
    return ESP_OK;
}

esp_err_t wl_unmount(wl_handle_t handle)
{
    last_unmount_handle = handle;
    return ESP_OK;
}

static int64_t fake_now_us;

int64_t esp_timer_get_time(void)
{
    return fake_now_us;
}

int main(void)
{
    reset_fakes();
    assert(storage_mount() == ESP_OK);
    assert(initial_mount_calls == 1);
    assert(s_wl == 1 && s_mounted);
    assert(format_calls == 0);

    assert(storage_unmount() == ESP_OK);
    assert(!s_mounted);
    assert(unregister_calls == 1);
    assert(detach_calls == 1);

    assert(storage_mount() == ESP_OK);
    assert(register_calls == 1);
    assert(s_mounted);
    assert(format_calls == 0);

    uint8_t data[4096] = {0};
    assert(storage_write_sector(5583, 0, data, sizeof(data)) == ESP_ERR_INVALID_STATE);
    assert(storage_unmount() == ESP_OK);
    assert(unregister_calls == 2);
    assert(detach_calls == 2);

    assert(storage_read_sector(5583, 0, data, sizeof(data)) == ESP_OK);
    assert(last_read_address == 5583 * 4096);
    assert(storage_write_sector(5583, 0, data, sizeof(data)) == ESP_OK);
    assert(last_erase_address == 5583 * 4096);
    assert(last_write_address == 5583 * 4096);

    assert(storage_read_sector(5584, 0, data, sizeof(data)) == ESP_ERR_INVALID_SIZE);
    assert(storage_read_sector(0, 0, data, 512) == ESP_ERR_INVALID_ARG);
    assert(storage_write_sector(5584, 0, data, sizeof(data)) == ESP_ERR_INVALID_SIZE);
    assert(storage_write_sector(0, 0, data, 0) == ESP_OK);
    assert(format_calls == 0);

    assert(storage_unmount() == ESP_OK);
    mount_result = ESP_FAIL;
    s_wl = WL_INVALID_HANDLE;
    s_mounted = false;
    assert(storage_mount() == ESP_FAIL);
    assert(!s_mounted);
    assert(s_wl == WL_INVALID_HANDLE);
    assert(last_unmount_handle == 1);
    assert(format_calls == 0);
    assert(take_calls == give_calls);

    // Returning a local mount to the host: never while held, never while recently used.
    reset_fakes();
    fake_now_us = 0;
    s_wl = WL_INVALID_HANDLE;
    s_mounted = false;
    s_holds = 0;
    assert(storage_release_to_host(2000000) == ESP_ERR_NOT_FOUND);
    assert(storage_mount() == ESP_OK);
    assert(storage_holds() & STORAGE_HOLD_REQUESTS);
    fake_now_us = 10000000;
    assert(storage_release_to_host(2000000) == ESP_ERR_INVALID_STATE);
    const uint32_t generation = storage_mount_generation();
    storage_requests_checked(generation + 1);
    assert(storage_holds() & STORAGE_HOLD_REQUESTS);
    storage_requests_checked(generation);
    assert(storage_holds() == 0);
    storage_hold(STORAGE_HOLD_ALBUM, true);
    assert(storage_release_to_host(2000000) == ESP_ERR_INVALID_STATE);
    storage_hold(STORAGE_HOLD_ALBUM, false);
    fake_now_us = 0;
    assert(storage_release_to_host(2000000) == ESP_ERR_INVALID_STATE);
    fake_now_us = 10000000;
    assert(storage_release_to_host(2000000) == ESP_OK);
    assert(!s_mounted);
    assert(format_calls == 0);
    assert(take_calls == give_calls);

    puts("storage mount and raw I/O tests passed");
    return 0;
}
