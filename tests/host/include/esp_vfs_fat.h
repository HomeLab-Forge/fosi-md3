#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ff.h"
#include "wear_levelling.h"

typedef struct {
    const char *base_path;
    const char *fat_drive;
    size_t max_files;
} esp_vfs_fat_conf_t;

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
} esp_vfs_fat_mount_config_t;

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(
    const char *base_path, const char *partition_label,
    const esp_vfs_fat_mount_config_t *mount_config, wl_handle_t *wl_handle);
esp_err_t esp_vfs_fat_unregister_path(const char *base_path);
esp_err_t esp_vfs_fat_register_cfg(const esp_vfs_fat_conf_t *conf, FATFS **out_fs);
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total, uint64_t *free_bytes);
