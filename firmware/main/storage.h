// The Photo Sync volume (the stock firmware's Photo1-3 and Game1-3 folders): the stock wear-levelled FAT on the "storage" partition, mounted at
// /disk and NEVER formatted (a failed mount leaves the volume untouched and reports an error).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define STORAGE_ROOT     "/disk"
#define STORAGE_MAX_FILE_BYTES (2 * 1024 * 1024)

/** Mount or reattach FAT/VFS without formatting. Keeps the WL handle alive across USB sessions. */
esp_err_t storage_mount(void);

/** Detach FAT/VFS for host raw access. Does not unmount the WL driver or format. */
esp_err_t storage_unmount(void);
bool storage_mounted(void);

/** Reasons the volume stays local. A new local mount sets STORAGE_HOLD_REQUESTS; the UI clears it
 *  once pending host requests are checked. (1U << 1) was the album's hold, removed with the album. */
#define STORAGE_HOLD_REQUESTS (1U << 0)
uint32_t storage_holds(void);

/** Local mount counter. Read it before checking requests, then pass it to storage_requests_checked,
 *  which clears STORAGE_HOLD_REQUESTS only if no newer mount happened meanwhile. */
uint32_t storage_mount_generation(void);
void storage_requests_checked(uint32_t generation);

/** Detach a locally mounted volume for the USB host if nothing holds it and it has been idle for
 *  idle_us. ESP_OK: released now. ESP_ERR_NOT_FOUND: already detached. ESP_ERR_INVALID_STATE:
 *  held or still in use. Never formats. */
esp_err_t storage_release_to_host(int64_t idle_us);

/** Read a whole file into PSRAM. Caller frees with free(). */
esp_err_t storage_read(const char *path, uint8_t **data, size_t *len);

/** Write a complete small file on the locally mounted volume, without formatting or raw-sector access. */
esp_err_t storage_write_file(const char *path, const uint8_t *data, size_t len);

/** Delete one file on the locally mounted volume. */
esp_err_t storage_remove(const char *path);

/** Free and total bytes on the volume; ESP_ERR_INVALID_STATE while detached. */
esp_err_t storage_usage(uint64_t *total, uint64_t *free_bytes);

/** Raw wear-levelled MSC access. All calls are bounds-checked and serialized with VFS work. */
uint32_t storage_sector_count(void);
uint32_t storage_sector_size(void);
esp_err_t storage_read_sector(uint32_t lba, uint32_t offset, void *buffer, size_t size);
esp_err_t storage_write_sector(uint32_t lba, uint32_t offset, const void *buffer, size_t size);
