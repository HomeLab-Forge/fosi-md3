// Settings record, byte-compatible with the stock firmware's 24-byte record in user_dat+0x1000,
// so switching between stock and custom firmware keeps the user's choices.
#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct __attribute__((packed)) {
    uint8_t magic;          // 0x00  0x55
    uint8_t uac1;           // 0x01  0 = UAC 2.0, 1 = UAC 1.0
    uint8_t gain_high;      // 0x02  0 = low (-6 dB), 1 = high
    uint8_t filter;         // 0x03  0..7, the stock firmware's FILTER_SHAPE; kept but unused here
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
    uint8_t eq_preset;      // 0x11  open firmware: EQ preset 0..8 (stock: padding, 0xFF -> Flat)
    uint8_t pad[2];         // 0x12
    // 0x14..0x17 are reserved in the stock record; the open firmware keeps the EQ coefficient scale
    // there, valid only while eq_scale_magic holds EQ_SCALE_MAGIC.
    uint8_t eq_scale_magic; // 0x14
    uint8_t eq_log2_2x;     // 0x15  a unity-gain 2x stage sums to 2^this
    uint8_t eq_log2_4x;     // 0x16  the same for the 4x stage
    int8_t eq_trim;         // 0x17  extra gain on the 2x stage, 0.1 dB steps
} md3_settings_t;

_Static_assert(sizeof(md3_settings_t) == 24, "settings record must stay 24 bytes");

#define EQ_SCALE_MAGIC 0xE9
#define SETTINGS_EQ_PRESETS 9  // must match EQ_PRESET_COUNT (checked in eq.c)

extern md3_settings_t g_settings;

/** Load the record; fall back to stock defaults if the magic byte is missing. */
esp_err_t settings_load(void);

/** Mark the record dirty; it is written 2 s after the last change, like the stock firmware. */
void settings_touch(void);

/** Call periodically (the UI task does, every 250 ms); performs the deferred write. */
void settings_tick(void);
