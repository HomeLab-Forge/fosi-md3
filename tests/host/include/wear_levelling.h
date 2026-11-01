#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE (-1)

size_t wl_size(wl_handle_t handle);
size_t wl_sector_size(wl_handle_t handle);
esp_err_t wl_unmount(wl_handle_t handle);
esp_err_t wl_read(wl_handle_t handle, size_t src_addr, void *dest, size_t size);
esp_err_t wl_write(wl_handle_t handle, size_t dest_addr, const void *src, size_t size);
esp_err_t wl_erase_range(wl_handle_t handle, size_t start_addr, size_t size);
