// EQ presets on the ES9039Q2M's programmable interpolation filter. Coefficients come from
// tools/eq_design.py (eq_coeffs.h), one set per sample-rate group. Call everything except the
// const getters and eq_scale_valid() from the UI task only: it owns the DAC.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    EQ_STATE_BUILTIN,   // preset 0 (Flat): the DAC's own filter F1
    EQ_STATE_CUSTOM,    // the preset's coefficients are loaded and in the signal path
    EQ_STATE_FALLBACK,  // the preset has no coefficients for this rate group; Flat is playing
    EQ_STATE_DSD,       // DSD bypasses the interpolation filter: no EQ
} eq_state_t;

typedef struct {
    int preset;
    int group;          // 0 = 44.1/48 kHz, 1 = 88.2/96, 2 = 176.4/192, 3 = 352.8/384
    eq_state_t state;
    int log2_2x;        // coefficient scale in use (see eq_set_scale)
    int log2_4x;
    int trim_tenth_db;
} eq_status_t;

/** Read the persisted scale from settings. Call once after settings_load(). */
void eq_init(void);

int eq_preset_count(void);
const char *eq_preset_name(int preset);
const char *eq_preset_note(int preset);

/** Response of a preset (quantized taps plus make-up) in 0.01 dB at eq_curve_points() log-spaced
 *  frequencies from eq_curve_f_min() to eq_curve_f_max(). All zero for Flat and for fallbacks. */
const int16_t *eq_curve(int preset, int group);
int eq_curve_points(void);
int eq_curve_f_min(void);
int eq_curve_f_max(void);

/** Bring the DAC in line with g_settings.eq_preset and the stream in g_link: reloads the coefficient
 *  RAM when the preset, rate group or scale changed or the DAC was reset, and sets the make-up. */
void eq_apply(void);

eq_status_t eq_status(void);

/** True if every coefficient set fits 24 bits at this scale. Pure; safe from any task. */
bool eq_scale_valid(int log2_2x, int log2_4x, int trim_tenth_db);

/** Set the unity scale (a unity-gain 2x stage sums to 2^log2_2x, the 4x stage to 2^log2_4x, and the
 *  2x stage gets trim_tenth_db of extra gain), persist it, and reload. ESP_ERR_INVALID_ARG if it
 *  does not fit. The default, 22/22/0, is Q1.22. */
esp_err_t eq_set_scale(int log2_2x, int log2_4x, int trim_tenth_db);
