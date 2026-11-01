#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef unsigned char BYTE;
typedef struct {
    int dummy;
} FATFS;

typedef enum {
    FR_OK = 0,
    FR_FAIL = 1,
    FR_INVALID_DRIVE = 2,
    FR_NO_FILESYSTEM = 3,
} FRESULT;

FRESULT f_mount(FATFS *fs, const char *path, BYTE opt);
FRESULT f_mkfs(const char *path, const void *opt, void *work, unsigned int len);
