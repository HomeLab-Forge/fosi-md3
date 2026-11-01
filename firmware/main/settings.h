// Settings record, byte-compatible with the stock firmware's 24-byte record in user_dat+0x1000,
// so switching between stock and custom firmware keeps the user's choices.
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct __attribute__((packed)) {
    uint8_t magic;          // 0x00  0x55
    uint8_t uac1;           // 0x01  0 = UAC 2.0, 1 = UAC 1.0
    uint8_t gain_high;      // 0x02  0 = low (-6 dB), 1 = high
    uint8_t filter;         // 0x03  0..7, written to DAC reg 0x58
    uint8_t vol_from_host;  // 0x04  1 = apply host volume and mute
    uint8_t usb_src;        // 0x05  logged only
    uint8_t udisk_mode;     // 0x06  0 = DAC, 1 = U-disk
    uint8_t brightness;     // 0x07  0..4 -> 20..100 %
    uint8_t rotation;       // 0x08  0..3 -> 0/90/180/270
    uint8_t saver_timeout;  // 0x09
    uint8_t saver_source;   // 0x0A
    uint8_t saver_poll;     // 0x0B
    uint8_t language;       // 0x0C
    uint8_t volume;         // 0x0D  0..100
    uint8_t volume_r;       // 0x0E  mirrors volume
    uint8_t album_folder;   // 0x0F  0..5 = Photo1-3, Game1-3
    uint8_t ask_language;   // 0x10
    uint8_t pad[3];         // 0x11
    uint32_t reserved;      // 0x14
} md3_settings_t;

_Static_assert(sizeof(md3_settings_t) == 24, "settings record must stay 24 bytes");

extern md3_settings_t g_settings;

/** Load the record; fall back to stock defaults if the magic byte is missing. */
esp_err_t settings_load(void);

/** Mark the record dirty; it is written 2 s after the last change, like the stock firmware. */
void settings_touch(void);

/** Call periodically (every ~40 ms); performs the deferred write. */
void settings_tick(void);
