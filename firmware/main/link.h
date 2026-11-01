// I2C slave link from the Savitech USB-audio bridge (address 0x6B). The bridge writes
// register/value pairs; nothing flows back. Registers used (docs/stock-firmware.md section 3):
//   0x10 bit1 = DSD stream, 0x11 = PCM rate code, 0x1E = native DSD rate,
//   0x20/0x21 = host volume L/R (signed dB), 0x22 = host mute.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t host_vol_l;
    uint8_t host_vol_r;
    bool host_mute;
    bool dsd;
    uint32_t rate_hz;       // PCM sample rate, or DSD bit rate / 1 (DSD64 = 2822400)
    char rate_label[16];    // "PCM 176.4k", "DSD128", "--"
    uint32_t writes;        // total register writes seen
} link_state_t;

extern link_state_t g_link;

esp_err_t link_init(void);

/** Fold any received writes into the state; returns true when something changed. */
bool link_poll(link_state_t *out);

/** Copy of the raw 256-byte register file (for diagnostics). */
void link_regfile(uint8_t out[256]);
