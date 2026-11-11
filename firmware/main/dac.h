// ES9039Q2M control over I2C, replaying the stock firmware's register sequence.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** Pulse reset, verify the chip ID, write the stock init table, then filter and volume. */
esp_err_t dac_init(void);

/** Recompute and write attenuation from settings + host volume/mute; skips unchanged values. */
void dac_apply_volume(void);

/** Set FILTER_SHAPE (reg 0x58 bits [2:0]) from settings, keeping the reserved bits. */
void dac_apply_filter(void);

/** Forget the cached attenuation so the next apply always writes. */
void dac_invalidate(void);

bool dac_present(void);
