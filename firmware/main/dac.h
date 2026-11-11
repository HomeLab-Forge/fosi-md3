// ES9039Q2M control over I2C, replaying the stock firmware's register sequence.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** Pulse reset, verify the chip ID, write the stock init table, then filter shape 0 and volume.
 *  The reset also clears the custom filter: see dac_reset_count(). */
esp_err_t dac_init(void);

/** Recompute and write attenuation from settings + host volume/mute; skips unchanged values. */
void dac_apply_volume(void);

/** Set FILTER_SHAPE (reg 0x58 bits [2:0]), keeping the reserved bits. */
void dac_set_filter_shape(uint8_t shape);

/** Load the programmable interpolation filter and switch to it (PROG_COEFF_EN = 1): mute and wait for
 *  the ramp, clear PROG_COEFF_EN, write each 24-bit coefficient (2x stage at 0x00.., 4x stage at
 *  0x80..), set PROG_COEFF_EN, then restore the volume less `makeup` 0.5 dB steps. On an I2C error
 *  the built-in filter stays selected and no make-up is applied. Blocks for about a second. */
esp_err_t dac_load_custom_filter(const int32_t *fir2x, size_t n2x, const int32_t *fir4x, size_t n4x, int makeup);

/** Back to a built-in filter (PROG_COEFF_EN = 0) with the given FILTER_SHAPE, muted around the change. */
esp_err_t dac_use_builtin_filter(uint8_t shape);

/** Change the EQ make-up (0.5 dB steps taken off the attenuation, never below 0 dB) and reapply. */
void dac_set_makeup(int steps);

/** Counts DAC resets; a change means the coefficient RAM and PROG_COEFF_EN were cleared. */
uint32_t dac_reset_count(void);

/** The device volume's attenuation in 0.5 dB steps (0xFF = mute), before host volume, gain and make-up. */
int dac_volume_attenuation(void);

/** Forget the cached attenuation so the next apply always writes. */
void dac_invalidate(void);

bool dac_present(void);
