#include "eq.h"

#include <math.h>
#include <stdlib.h>

#include "dac.h"
#include "eq_coeffs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "link.h"
#include "settings.h"

static const char *TAG = "eq";

_Static_assert(EQ_PRESET_COUNT == SETTINGS_EQ_PRESETS, "settings.h and eq_coeffs.h disagree on the preset count");

#define COEF_MAX        ((1 << 23) - 1)
#define LOG2_MIN        14
#define LOG2_MAX        24
#define TRIM_MAX        120   // 0.1 dB steps: +-12 dB
#define LOADED_NONE     (-2)  // forces the next eq_apply() to program the DAC

static int s_log2_2x = EQ_COEF_UNITY_LOG2;
static int s_log2_4x = EQ_COEF_UNITY_LOG2;
static int s_trim;
static int s_group;                  // last PCM rate group seen; 44.1/48 until a stream says otherwise
static bool s_dsd;
static int s_loaded = LOADED_NONE;   // k_eq_fir2x index in the DAC, -1 = built-in filter
static uint32_t s_seen_reset;
static int32_t s_max_2x, s_max_4x;   // largest |coefficient| in the tables, at unity scale
static int32_t s_buf2x[EQ_FIR2X_TAPS];
static int32_t s_buf4x[EQ_FIR4X_TAPS];

static int preset_index(void)
{
    return g_settings.eq_preset < EQ_PRESET_COUNT ? g_settings.eq_preset : 0;
}

static double scale_2x(int log2_2x, int trim_tenth_db)
{
    return ldexp(1.0, log2_2x - EQ_COEF_UNITY_LOG2) * pow(10.0, trim_tenth_db / 200.0);
}

static double scale_4x(int log2_4x)
{
    return ldexp(1.0, log2_4x - EQ_COEF_UNITY_LOG2);
}

void eq_init(void)
{
    for (int s = 0; s < EQ_FIR2X_SETS; s++) {
        for (int i = 0; i < EQ_FIR2X_TAPS; i++) {
            const int32_t v = abs(k_eq_fir2x[s][i]);
            s_max_2x = v > s_max_2x ? v : s_max_2x;
        }
    }
    for (int i = 0; i < EQ_FIR4X_TAPS; i++) {
        const int32_t v = abs(k_eq_fir4x[i]);
        s_max_4x = v > s_max_4x ? v : s_max_4x;
    }
    if (g_settings.eq_scale_magic == EQ_SCALE_MAGIC &&
        eq_scale_valid(g_settings.eq_log2_2x, g_settings.eq_log2_4x, g_settings.eq_trim)) {
        s_log2_2x = g_settings.eq_log2_2x;
        s_log2_4x = g_settings.eq_log2_4x;
        s_trim = g_settings.eq_trim;
    }
    ESP_LOGI(TAG, "preset %s, scale 2^%d / 2^%d, trim %+d.%d dB", k_eq_name[preset_index()], s_log2_2x,
             s_log2_4x, s_trim / 10, abs(s_trim) % 10);
}

int eq_preset_count(void)
{
    return EQ_PRESET_COUNT;
}

const char *eq_preset_name(int preset)
{
    return k_eq_name[preset >= 0 && preset < EQ_PRESET_COUNT ? preset : 0];
}

const char *eq_preset_note(int preset)
{
    return k_eq_note[preset >= 0 && preset < EQ_PRESET_COUNT ? preset : 0];
}

const int16_t *eq_curve(int preset, int group)
{
    if (preset < 0 || preset >= EQ_PRESET_COUNT) {
        preset = 0;
    }
    if (group < 0 || group >= EQ_GROUP_COUNT) {
        group = 0;
    }
    return k_eq_curve[group][preset];
}

int eq_curve_points(void)
{
    return EQ_CURVE_POINTS;
}

int eq_curve_f_min(void)
{
    return EQ_CURVE_F_MIN;
}

int eq_curve_f_max(void)
{
    return EQ_CURVE_F_MAX;
}

static int group_for_rate(uint32_t hz)
{
    return hz <= 50000 ? 0 : hz <= 100000 ? 1 : hz <= 200000 ? 2 : 3;
}

bool eq_scale_valid(int log2_2x, int log2_4x, int trim_tenth_db)
{
    if (log2_2x < LOG2_MIN || log2_2x > LOG2_MAX || log2_4x < LOG2_MIN || log2_4x > LOG2_MAX ||
        trim_tenth_db < -TRIM_MAX || trim_tenth_db > TRIM_MAX) {
        return false;
    }
    // s_max_* are filled by eq_init() at boot, before the USB control channel can ask.
    return s_max_2x * scale_2x(log2_2x, trim_tenth_db) <= COEF_MAX && s_max_4x * scale_4x(log2_4x) <= COEF_MAX;
}

static esp_err_t load_set(int set, int makeup)
{
    const double k2 = scale_2x(s_log2_2x, s_trim);
    const double k4 = scale_4x(s_log2_4x);
    for (int i = 0; i < EQ_FIR2X_TAPS; i++) {
        s_buf2x[i] = (int32_t)lround(k_eq_fir2x[set][i] * k2);
    }
    for (int i = 0; i < EQ_FIR4X_TAPS; i++) {
        s_buf4x[i] = (int32_t)lround(k_eq_fir4x[i] * k4);
    }
    return dac_load_custom_filter(s_buf2x, EQ_FIR2X_TAPS, s_buf4x, EQ_FIR4X_TAPS, makeup);
}

void eq_apply(void)
{
    if (!dac_present()) {
        return;
    }
    if (g_link.rate_hz && !g_link.dsd) {
        s_group = group_for_rate(g_link.rate_hz);
    }
    s_dsd = g_link.dsd;
    const int preset = preset_index();
    const int want = k_eq_set[s_group][preset];
    if (dac_reset_count() != s_seen_reset) {
        s_seen_reset = dac_reset_count();
        s_loaded = LOADED_NONE;
    }
    if (want != s_loaded) {
        if (want < 0) {
            dac_use_builtin_filter(0);
            s_loaded = -1;
            ESP_LOGI(TAG, "%s: built-in filter F1 (group %d)", k_eq_name[preset], s_group);
        } else {
            const int64_t t0 = esp_timer_get_time();
            if (load_set(want, s_dsd ? 0 : k_eq_makeup[preset]) == ESP_OK) {
                s_loaded = want;
                ESP_LOGI(TAG, "%s: loaded set %d for group %d in %lld ms", k_eq_name[preset], want, s_group,
                         (esp_timer_get_time() - t0) / 1000);
            } else {
                s_loaded = -1;  // dac_load_custom_filter left the built-in filter selected
            }
        }
    }
    // DSD skips the interpolator, so the level it would have given back must not be added.
    dac_set_makeup(s_loaded >= 0 && !s_dsd ? k_eq_makeup[preset] : 0);
}

eq_status_t eq_status(void)
{
    const int preset = preset_index();
    eq_state_t state;
    if (s_dsd) {
        state = EQ_STATE_DSD;
    } else if (preset == 0) {
        state = EQ_STATE_BUILTIN;
    } else if (k_eq_set[s_group][preset] < 0) {
        state = EQ_STATE_FALLBACK;
    } else {
        state = EQ_STATE_CUSTOM;
    }
    return (eq_status_t){
        .preset = preset,
        .group = s_group,
        .state = state,
        .log2_2x = s_log2_2x,
        .log2_4x = s_log2_4x,
        .trim_tenth_db = s_trim,
    };
}

esp_err_t eq_set_scale(int log2_2x, int log2_4x, int trim_tenth_db)
{
    if (!eq_scale_valid(log2_2x, log2_4x, trim_tenth_db)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_log2_2x = log2_2x;
    s_log2_4x = log2_4x;
    s_trim = trim_tenth_db;
    g_settings.eq_scale_magic = EQ_SCALE_MAGIC;
    g_settings.eq_log2_2x = (uint8_t)log2_2x;
    g_settings.eq_log2_4x = (uint8_t)log2_4x;
    g_settings.eq_trim = (int8_t)trim_tenth_db;
    settings_touch();
    ESP_LOGI(TAG, "scale 2^%d / 2^%d, trim %d (0.1 dB)", log2_2x, log2_4x, trim_tenth_db);
    s_loaded = LOADED_NONE;
    eq_apply();
    return ESP_OK;
}
