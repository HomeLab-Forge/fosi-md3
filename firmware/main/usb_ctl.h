// Vendor control requests on USB endpoint 0: the host's side channel to the firmware. They work
// while the host has the Photo Sync volume mounted, so nothing here ejects or remounts it.
//
// bmRequestType 0xC0 (vendor, device, IN) or 0x40 (vendor, device, OUT). OUT requests carry no
// data stage; their arguments ride in wValue and wIndex. Unknown or malformed requests stall.
#pragma once

#include "esp_err.h"

#define MD3_CTL_INFO   0x01  // IN: status JSON
// 0x02 (TIME) and 0x09 (SUN) set the day-wheel clock, which is gone; they now stall.
#define MD3_CTL_KEY    0x03  // OUT: wValue = md3_key_t, wIndex = md3_press_t
#define MD3_CTL_SHOT   0x04  // IN: capture the screen as BMP; returns its size, u32 little-endian
#define MD3_CTL_READ   0x05  // IN: read the captured BMP at offset (wIndex << 16) | wValue
#define MD3_CTL_LOG    0x06  // IN: newest log text, up to wLength bytes
#define MD3_CTL_OFFER  0x07  // OUT: return a device-held volume to the host now (no-op if the host has it)
#define MD3_CTL_ROM    0x08  // OUT: reboot to ROM download mode; wValue 0x524D, wIndex 0x3344
#define MD3_CTL_EQ_SCALE 0x0A  // OUT: EQ coefficient scale: wValue = log2_2x | log2_4x << 8 (a unity-gain
                              // stage sums to 2^n; 22 = Q1.22), wIndex = int16 trim on the 2x stage in 0.1 dB.
                              // Persisted; stalls if any coefficient would overflow 24 bits.

#define MD3_CTL_ROM_VALUE 0x524D
#define MD3_CTL_ROM_INDEX 0x3344

/** Allocate reply buffers. Called once after the TinyUSB driver is installed. */
esp_err_t usb_ctl_init(void);
