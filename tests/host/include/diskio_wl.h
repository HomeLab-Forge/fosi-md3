#pragma once

#include "wear_levelling.h"

unsigned char ff_diskio_get_pdrv_wl(wl_handle_t flash_handle);
void ff_diskio_clear_pdrv_wl(wl_handle_t flash_handle);
esp_err_t ff_diskio_register_wl_partition(unsigned char pdrv, wl_handle_t flash_handle);
