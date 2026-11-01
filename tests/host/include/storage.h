#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t storage_mount(void);
esp_err_t storage_unmount(void);
bool storage_mounted(void);
uint32_t storage_sector_count(void);
uint32_t storage_sector_size(void);
esp_err_t storage_read_sector(uint32_t lba, uint32_t offset, void *buffer, size_t size);
esp_err_t storage_write_sector(uint32_t lba, uint32_t offset, const void *buffer, size_t size);
