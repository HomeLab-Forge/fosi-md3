#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "dac.h"
#include "eq.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "link.h"
#include "md3.h"
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "tinyusb.h"
#include "usb_msc.h"

static const char *TAG = "ui";

// Colours from design/status-screen.html. The panel inverts colours in hardware; board.c sets that.
#define COLOR_BG        lv_color_hex(0x000000)
#define COLOR_CURVE     lv_color_hex(0x7FD4FF)
#define COLOR_CURVE_DIM lv_color_hex(0x2B4A5C)
#define COLOR_RING      lv_color_hex(0xF2B94B)
#define COLOR_TRACK     lv_color_hex(0x24282D)
#define COLOR_LABEL     lv_color_hex(0xE7EDF2)
#define COLOR_SUB       lv_color_hex(0x8A98A5)
#define COLOR_DSD       lv_color_hex(0xC79BFF)
#define COLOR_GRID      lv_color_hex(0x15212A)

#define HOLD_REPEAT_US  70000
#define HOT_MS          1500                  // the big dB value after a volume change
#define AWAKE_US        (8 * 1000 * 1000)     // no key for this long: screen off
#define HOUSEKEEPING_MS 250                   // VBUS, deferred saves, drive return, sleep timer, link fallback
#define AMP_IDLE_US     (30 * 1000000LL)      // automuted this long: headphone amps off
#define AMP_POLL_MS     20                    // automute poll while the amps are off, so sound returns quickly
#define GC9A01_SLPIN    0x10
#define GC9A01_SLPOUT   0x11
#define GC9A01_DISPOFF  0x28
#define GC9A01_DISPON   0x29
#define GC9A01_SLEEP_GAP_US (120 * 1000)      // GC9A01: 120 ms between sleep in and sleep out, both ways

// Power estimate, mW. Datasheet figures per chip, plus a fixed part (USB bridge, ESP32, converters)
// calibrated so the model gives the iOS reading of 0.3.1: 6.6 %/h of an iPhone 15 Pro's 12.7 Wh
// while playing with the screen dark. The time in each state is exact; the watts are an estimate,
// about +-25%.
#define PWR_FIXED_MW      706
#define PWR_DAC_PLAY_MW   54    // DAC not automuted
#define PWR_DAC_MUTE_MW   15    // DAC automuted (or no stream)
#define PWR_AMPS_MW       80    // headphone amps on
#define PWR_SCREEN_MW     150   // backlight and panel on
#define PWR_BATTERY_MWH   12700 // iPhone 15 Pro

// Curve box: 200 Hz..20 kHz on a log axis from x 36 to 204, 0 dB at y 112, 8 px per dB.
#define CURVE_LEFT      36
#define CURVE_RIGHT     204
#define CURVE_MID       112
#define CURVE_PX_PER_DB 6
#define CURVE_MAX_DB    7

static QueueHandle_t s_queue;
static lv_display_t *s_disp;
static esp_lcd_panel_io_handle_t s_io;
static esp_pm_lock_handle_t s_cpu_lock;  // full CPU speed while the screen is on
static volatile bool s_link_signalled;   // an EV_LINK wake-up is already queued
static int64_t s_last_hold_us;

// Wake and sleep. The screen is dark (backlight 0, panel in sleep, LVGL stopped) unless Menu woke it.
static bool s_awake;
static int64_t s_last_key_us;
static bool s_amp_off;
static int64_t s_muted_since;   // 0 = not automuted
static uint32_t s_amp_resets;   // dac_reset_count() when the amps were last switched
static int64_t s_panel_toggle_us;     // last sleep-in or sleep-out
static bool s_dac_muted = true;       // what amp_idle_tick last saw: automuted or no stream

// Time in each state since boot, us; account_power() adds the time since the last call.
static int64_t s_acct_us, s_acct_screen_us, s_acct_amp_off_us, s_acct_muted_us, s_acct_stream_us;

// Work that talks to the DAC for long; done by the UI task after it lets go of the LVGL lock.
static bool s_pending_eq;
static bool s_pending_power_cycle;

static lv_obj_t *s_scr, *s_arc, *s_stream, *s_gain, *s_grid, *s_curve, *s_name, *s_note, *s_power, *s_db, *s_big;
static lv_point_precise_t s_grid_points[2];
static lv_point_precise_t *s_curve_points;
static lv_timer_t *s_hot_timer;

static uint64_t s_storage_total;
static uint64_t s_storage_free;
static int64_t s_storage_read_us;

bool md3_post(const md3_event_t *ev)
{
    return s_queue && xQueueSend(s_queue, ev, 0) == pdTRUE;
}

// ------------------------------------------------------------------ helpers

static const int k_brightness_pct[5] = {20, 40, 60, 80, 100};

// Call before any of the states it counts changes, so the time so far goes to the old state.
static void account_power(void)
{
    const int64_t now = esp_timer_get_time();
    const int64_t dt = now - s_acct_us;
    s_acct_us = now;
    if (dt <= 0) {
        return;
    }
    if (s_awake) {
        s_acct_screen_us += dt;
    }
    if (s_amp_off) {
        s_acct_amp_off_us += dt;
    }
    if (s_dac_muted) {
        s_acct_muted_us += dt;
    }
    if (g_link.rate_hz) {
        s_acct_stream_us += dt;
    }
}

static void power_estimate(md3_power_est_t *out)
{
    account_power();
    const int64_t t = s_acct_us > 0 ? s_acct_us : 1;
    // mW x us = nJ
    const int64_t nj = PWR_FIXED_MW * t
                     + PWR_DAC_PLAY_MW * (t - s_acct_muted_us) + PWR_DAC_MUTE_MW * s_acct_muted_us
                     + PWR_AMPS_MW * (t - s_acct_amp_off_us)
                     + PWR_SCREEN_MW * s_acct_screen_us;
    *out = (md3_power_est_t){
        .avg_mw = (uint32_t)((nj + t / 2) / t),
        .mwh = (uint32_t)(nj / 3600000000LL),
        .elapsed_s = (uint32_t)(t / 1000000),
        .screen_on_pct = (uint8_t)(s_acct_screen_us * 100 / t),
        .amp_off_pct = (uint8_t)(s_acct_amp_off_us * 100 / t),
        .muted_pct = (uint8_t)(s_acct_muted_us * 100 / t),
        .stream_pct = (uint8_t)(s_acct_stream_us * 100 / t),
    };
    out->pct_per_h_x10 = (out->avg_mw * 1000 + PWR_BATTERY_MWH / 2) / PWR_BATTERY_MWH;  // %/h x10
}

static void publish_state(void)
{
    const int64_t now = esp_timer_get_time();
    if (storage_mounted() && (!s_storage_read_us || now - s_storage_read_us >= 30000000)) {
        if (storage_usage(&s_storage_total, &s_storage_free) == ESP_OK) {
            s_storage_read_us = now;
        }
    }
    md3_snapshot_t snapshot = {
        .settings = g_settings,
        .link = g_link,
        .eq = eq_status(),
        .dac_present = dac_present(),
        .amp_on = !s_amp_off,
        .power_vbus_mv = power_vbus_mv(),
        .power_charging = power_charging(),
        .storage_mounted = storage_mounted(),
        .storage_total = s_storage_total,
        .storage_free = s_storage_free,
        .storage_holds = storage_holds(),
    };
    power_estimate(&snapshot.power_est);
    strlcpy(snapshot.screen, ui_screen_name(), sizeof(snapshot.screen));
    md3_state_publish(&snapshot);
}

static void apply_rotation(void)
{
    static const lv_display_rotation_t rot[4] = {
        LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270,
    };
    // Panel MY handles the hardware reflection; the saved user rotation applies directly.
    lv_display_set_rotation(s_disp, rot[g_settings.rotation & 3]);
}

static lv_obj_t *add_label(const lv_font_t *font, lv_color_t color, int y)
{
    lv_obj_t *l = lv_label_create(s_scr);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// ------------------------------------------------------------------ the status screen

static void refresh_stream(void)
{
    if (g_link.dsd) {
        lv_label_set_text(s_stream, g_link.rate_hz ? g_link.rate_label : "DSD");
        lv_obj_set_style_text_color(s_stream, COLOR_DSD, 0);
    } else if (g_link.rate_hz) {
        const unsigned k = g_link.rate_hz / 1000, frac = g_link.rate_hz % 1000 / 100;
        if (frac) {
            lv_label_set_text_fmt(s_stream, "%u.%u kHz PCM", k, frac);
        } else {
            lv_label_set_text_fmt(s_stream, "%u kHz PCM", k);
        }
        lv_obj_set_style_text_color(s_stream, COLOR_LABEL, 0);
    } else {
        lv_label_set_text(s_stream, "no stream");
        lv_obj_set_style_text_color(s_stream, COLOR_SUB, 0);
    }
}

static void refresh_eq(void)
{
    const eq_status_t eq = eq_status();
    const int16_t *curve = eq_curve(eq.preset, eq.group);
    const int n = eq_curve_points();
    for (int i = 0; i < n; i++) {
        int cdb = curve[i];
        cdb = cdb > CURVE_MAX_DB * 100 ? CURVE_MAX_DB * 100 : cdb < -CURVE_MAX_DB * 100 ? -CURVE_MAX_DB * 100 : cdb;
        s_curve_points[i].x = CURVE_LEFT + (CURVE_RIGHT - CURVE_LEFT) * i / (n - 1);
        s_curve_points[i].y = CURVE_MID - cdb * CURVE_PX_PER_DB / 100;
    }
    lv_line_set_points(s_curve, s_curve_points, n);
    const bool dsd = eq.state == EQ_STATE_DSD;
    lv_obj_set_style_line_color(s_curve, dsd ? COLOR_CURVE_DIM : COLOR_CURVE, 0);
    lv_label_set_text(s_name, eq_preset_name(eq.preset));
    lv_obj_set_style_text_color(s_name, dsd ? COLOR_SUB : COLOR_LABEL, 0);
    lv_label_set_text(s_note, dsd ? "EQ not used on DSD"
                              : eq.state == EQ_STATE_FALLBACK ? "Flat at this rate"
                              : eq_preset_note(eq.preset));
}

static void format_db(char *buf, size_t n)
{
    const int att = dac_volume_attenuation();  // 0.5 dB steps
    if (att >= 0xFF || (g_settings.vol_from_host && g_link.host_mute)) {
        snprintf(buf, n, "mute");
    } else {
        snprintf(buf, n, "%s%d.%d dB", att ? "-" : "", att / 2, att % 2 ? 5 : 0);
    }
}

static void refresh_volume(void)
{
    char buf[16];
    format_db(buf, sizeof(buf));
    lv_arc_set_value(s_arc, g_settings.volume);
    lv_label_set_text(s_db, buf);
    lv_label_set_text(s_big, buf);
    lv_label_set_text(s_gain, g_settings.gain_high ? "High Gain" : "Low Gain");
}

// The montserrat built-ins hold ASCII, the degree sign and the bullet (U+2022) only: no
// approximately-equal sign or middle dot, so "~" and the bullet stand in for them.
static void refresh_power(void)
{
    static char shown[48];
    md3_power_est_t p;
    power_estimate(&p);
    const uint32_t cw = (p.avg_mw + 5) / 10;  // centiwatts
    char buf[48];
    snprintf(buf, sizeof(buf), "~%lu.%02lu W  \xe2\x80\xa2  %lu.%lu %%/h", (unsigned long)(cw / 100),
             (unsigned long)(cw % 100), (unsigned long)(p.pct_per_h_x10 / 10),
             (unsigned long)(p.pct_per_h_x10 % 10));
    if (strcmp(buf, shown)) {
        strlcpy(shown, buf, sizeof(shown));
        lv_label_set_text(s_power, buf);
    }
}

static void refresh_all(void)
{
    refresh_stream();
    refresh_eq();
    refresh_volume();
    refresh_power();
}

// While "hot" the big value replaces the preset name, note and small dB, and the ring brightens.
static void set_hot(bool hot)
{
    lv_obj_update_flag(s_big, LV_OBJ_FLAG_HIDDEN, !hot);
    lv_obj_update_flag(s_name, LV_OBJ_FLAG_HIDDEN, hot);
    lv_obj_update_flag(s_note, LV_OBJ_FLAG_HIDDEN, hot);
    lv_obj_update_flag(s_power, LV_OBJ_FLAG_HIDDEN, hot);
    lv_obj_update_flag(s_db, LV_OBJ_FLAG_HIDDEN, hot);
    lv_obj_set_style_arc_opa(s_arc, hot ? LV_OPA_COVER : LV_OPA_80, LV_PART_INDICATOR);
}

static void hot_expired(lv_timer_t *t)
{
    set_hot(false);
    lv_timer_pause(t);
}

// Show text in the big slot for HOT_MS: the volume, or a mode change.
static void show_hot(const char *text)
{
    if (text) {
        lv_label_set_text(s_big, text);
    }
    set_hot(true);
    lv_timer_reset(s_hot_timer);
    lv_timer_resume(s_hot_timer);
}

static void build_screen(void)
{
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Volume ring from 7 to 5 o'clock (LVGL angles run clockwise from 3 o'clock).
    s_arc = lv_arc_create(s_scr);
    lv_obj_set_size(s_arc, 228, 228);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_RING, LV_PART_INDICATOR);

    s_stream = add_label(&lv_font_montserrat_16, COLOR_LABEL, 34);
    s_gain = add_label(&lv_font_montserrat_14, COLOR_SUB, 53);  // under the stream line

    s_grid_points[0] = (lv_point_precise_t){CURVE_LEFT, CURVE_MID};
    s_grid_points[1] = (lv_point_precise_t){CURVE_RIGHT, CURVE_MID};
    s_grid = lv_line_create(s_scr);
    lv_line_set_points(s_grid, s_grid_points, 2);
    lv_obj_set_style_line_width(s_grid, 1, 0);
    lv_obj_set_style_line_color(s_grid, COLOR_GRID, 0);
    lv_obj_set_pos(s_grid, 0, 0);

    s_curve = lv_line_create(s_scr);
    lv_obj_set_style_line_width(s_curve, 2, 0);
    lv_obj_set_style_line_rounded(s_curve, true, 0);
    lv_obj_set_pos(s_curve, 0, 0);

    // Bottom block: four centred lines, 4 px apart.
    s_name = add_label(&lv_font_montserrat_16, COLOR_LABEL, 150);
    s_note = add_label(&lv_font_montserrat_14, COLOR_SUB, 170);
    s_power = add_label(&lv_font_montserrat_12, COLOR_SUB, 188);
    s_db = add_label(&lv_font_montserrat_14, COLOR_RING, 204);
    s_big = add_label(&lv_font_montserrat_28, COLOR_RING, 160);
    lv_obj_add_flag(s_big, LV_OBJ_FLAG_HIDDEN);

    s_hot_timer = lv_timer_create(hot_expired, HOT_MS, NULL);
    lv_timer_pause(s_hot_timer);
    refresh_all();
    lv_screen_load(s_scr);
}

// ------------------------------------------------------------------ wake and sleep

static void panel_cmd(uint8_t cmd)
{
    // tx_param waits for any colour transfer still in flight, so this is safe right after a flush.
    if (esp_lcd_panel_io_tx_param(s_io, cmd, NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "panel command 0x%02x failed", cmd);
    }
}

static void panel_gap(void)
{
    const int64_t since = esp_timer_get_time() - s_panel_toggle_us;
    if (since < GC9A01_SLEEP_GAP_US) {
        vTaskDelay(pdMS_TO_TICKS((GC9A01_SLEEP_GAP_US - since) / 1000 + 1));
    }
}

// Call with the LVGL lock held.
static void screen_wake(void)
{
    if (s_awake) {
        return;
    }
    panel_gap();
    panel_cmd(GC9A01_SLPOUT);
    vTaskDelay(pdMS_TO_TICKS(120));  // sleep-out settles before the next command and before any draw
    s_panel_toggle_us = esp_timer_get_time();
    panel_cmd(GC9A01_DISPON);
    lvgl_port_resume();
    set_hot(false);
    refresh_all();
    lv_obj_invalidate(s_scr);
    lv_refr_now(s_disp);
    vTaskDelay(pdMS_TO_TICKS(20));   // let the last DMA flush land before the light comes on
    board_backlight_set(k_brightness_pct[g_settings.brightness % 5]);
    if (s_cpu_lock) {
        esp_pm_lock_acquire(s_cpu_lock);
    }
    account_power();
    s_awake = true;
    s_last_key_us = esp_timer_get_time();
    ESP_LOGI(TAG, "screen on");
}

// Call with the LVGL lock held.
static void screen_sleep(void)
{
    if (!s_awake) {
        return;
    }
    board_backlight_set(0);
    lv_timer_pause(s_hot_timer);
    set_hot(false);
    lvgl_port_stop();  // no tick, no LVGL timers: nothing redraws until the next wake
    if (s_cpu_lock) {
        esp_pm_lock_release(s_cpu_lock);
    }
    panel_gap();
    panel_cmd(GC9A01_DISPOFF);
    panel_cmd(GC9A01_SLPIN);
    s_panel_toggle_us = esp_timer_get_time();
    account_power();
    s_awake = false;
    ESP_LOGI(TAG, "screen off");
}

// ------------------------------------------------------------------ events

static void volume_step(int delta)
{
    int v = g_settings.volume + delta;
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v == g_settings.volume) {
        return;
    }
    g_settings.volume = v;
    settings_touch();
    dac_apply_volume();
    if (s_awake) {
        refresh_volume();
        show_hot(NULL);
    }
}

static void handle_key(md3_key_t key, md3_press_t press)
{
    if (press == PRESS_HOLD) {
        const int64_t now = esp_timer_get_time();
        if (now - s_last_hold_us < HOLD_REPEAT_US) {
            return;
        }
        s_last_hold_us = now;
    }
    if (s_awake) {
        s_last_key_us = esp_timer_get_time();
    }
    switch (key) {
    case KEY_VOL_UP:
    case KEY_VOL_DOWN:
        // Volume works with the screen off and does not wake it.
        if (press != PRESS_LONG) {
            volume_step(key == KEY_VOL_UP ? 1 : -1);
        }
        break;
    case KEY_MENU:
        if (!s_awake) {
            screen_wake();  // from dark, any press only wakes
        } else if (press == PRESS_CLICK) {
            screen_sleep();
        } else if (press == PRESS_LONG) {
            // DAC (Savitech on the port) <-> U-Disk (Photo Sync); the old menu's USB mode item.
            g_settings.udisk_mode ^= 1;
            settings_touch();
            s_pending_power_cycle = true;
            show_hot(g_settings.udisk_mode ? "U-Disk" : "DAC");
        }
        break;
    case KEY_VISTA:
        // Only while the screen is on: a click picks the next preset, a hold switches the gain.
        if (!s_awake) {
            break;
        }
        if (press == PRESS_CLICK) {
            g_settings.eq_preset = (g_settings.eq_preset + 1) % eq_preset_count();
            settings_touch();
            s_pending_eq = true;
            set_hot(false);
            refresh_eq();
        } else if (press == PRESS_LONG) {
            g_settings.gain_high ^= 1;
            settings_touch();
            dac_apply_volume();
            refresh_volume();
            show_hot(g_settings.gain_high ? "High Gain" : "Low Gain");
        }
        break;
    }
}

static void handle_event(md3_event_t *ev)
{
    switch (ev->type) {
    case EV_KEY:
        handle_key(ev->key, ev->press);
        break;
    case EV_LINK:
        dac_apply_volume();
        s_pending_eq = true;  // a new rate group needs its own coefficients
        if (s_awake) {
            refresh_all();
        }
        break;
    case EV_EQ_SCALE:
        if (eq_set_scale(ev->value & 0xFF, (ev->value >> 8) & 0xFF, (int16_t)(ev->value >> 16)) != ESP_OK) {
            ESP_LOGW(TAG, "EQ scale 0x%08lx rejected", (unsigned long)ev->value);
        }
        break;
    case EV_OFFER:
    case EV_ROM:
        break;  // handled by ui_task outside the LVGL lock
    }
}

// The DAC work an event asked for, outside the LVGL lock so LVGL can draw meanwhile.
static void run_pending(void)
{
    if (s_pending_power_cycle) {
        s_pending_power_cycle = false;
        if (audio_power_cycle() != ESP_OK) {
            ESP_LOGW(TAG, "DAC not ready after the mode switch");
        }
        s_pending_eq = true;  // the DAC reset dropped the coefficients
    }
    if (s_pending_eq) {
        s_pending_eq = false;
        eq_apply();
        if (s_awake && lvgl_port_lock(0)) {
            refresh_eq();  // the rate group or a load failure can change what is shown
            lvgl_port_unlock();
        }
    }
}

static void process_event(md3_event_t *ev)
{
    if (lvgl_port_lock(0)) {
        handle_event(ev);
        lvgl_port_unlock();
    }
    run_pending();
    publish_state();
}

// A host asks for ROM download mode by writing ROM_REQUEST_PATH, starting with the marker, to
// the Photo Sync volume and ejecting it. The file is removed before the reboot, so it fires once.
#define ROM_REQUEST_PATH   STORAGE_ROOT "/MD3ROM.TXT"
#define ROM_REQUEST_MARKER "md3-enter-rom"

// After a request the host sees its answer at once: detach and reconnect.
static void return_after_request(void)
{
    if (storage_unmount() != ESP_OK) {
        return;
    }
    usb_msc_reconnect();
}

static void reboot_to_rom(void)
{
    if (lvgl_port_lock(0)) {
        lv_label_set_text(s_note, "ROM download mode");
        if (s_awake) {
            lv_refr_now(s_disp);
        }
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(300));  // let the panel show the notice and the host see the status stage
    // Drop the pull-up first. Without a visible disconnect the host keeps addressing the old
    // device after the PHY moves to USB-Serial-JTAG, and finds the ROM only after its own
    // error recovery resets the port, which can take minutes.
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    board_reboot_to_download();
}

static void check_rom_request(void)
{
    uint8_t *data = NULL;
    size_t len = 0;
    if (storage_read(ROM_REQUEST_PATH, &data, &len) != ESP_OK) {
        return;
    }
    const size_t marker_len = strlen(ROM_REQUEST_MARKER);
    const bool requested = len >= marker_len && !memcmp(data, ROM_REQUEST_MARKER, marker_len);
    free(data);
    if (!requested) {
        ESP_LOGW(TAG, "ignoring %s without the request marker", ROM_REQUEST_PATH);
        return;
    }
    if (storage_remove(ROM_REQUEST_PATH) != ESP_OK) {
        ESP_LOGE(TAG, "cannot remove %s; staying in the app", ROM_REQUEST_PATH);
        return;
    }
    reboot_to_rom();
}

#define SHOT_REQUEST_PATH   STORAGE_ROOT "/MD3SHOT.TXT"
#define SHOT_REQUEST_MARKER "md3-screenshot"
#define SHOT_OUTPUT_PATH    STORAGE_ROOT "/MD3SHOT.BMP"

static void check_screenshot_request(void)
{
    uint8_t *request = NULL;
    size_t request_len = 0;
    if (storage_read(SHOT_REQUEST_PATH, &request, &request_len) != ESP_OK) {
        return;
    }
    const size_t marker_len = strlen(SHOT_REQUEST_MARKER);
    const bool requested = request_len >= marker_len && !memcmp(request, SHOT_REQUEST_MARKER, marker_len);
    free(request);
    if (!requested || storage_remove(SHOT_REQUEST_PATH) != ESP_OK) {
        return;
    }

    uint8_t *bmp = NULL;
    size_t bmp_len = 0;
    esp_err_t result = ui_screenshot_bmp(&bmp, &bmp_len);
    if (result == ESP_OK) {
        result = storage_write_file(SHOT_OUTPUT_PATH, bmp, bmp_len);
    }
    free(bmp);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "screenshot-over-USB failed: %s", esp_err_to_name(result));
        return;
    }
    return_after_request();
}

#define KEY_REQUEST_PATH   STORAGE_ROOT "/MD3KEY.TXT"
#define KEY_REQUEST_MARKER "md3-key="

static void check_key_request(void)
{
    uint8_t *request = NULL;
    size_t request_len = 0;
    if (storage_read(KEY_REQUEST_PATH, &request, &request_len) != ESP_OK) {
        return;
    }
    char payload[40] = {0};
    const size_t marker_len = strlen(KEY_REQUEST_MARKER);
    const size_t payload_len = request_len >= marker_len ? request_len - marker_len : 0;
    if (payload_len && payload_len < sizeof(payload) - 1 &&
        !memcmp(request, KEY_REQUEST_MARKER, marker_len)) {
        memcpy(payload, request + marker_len, payload_len);
    }
    free(request);
    payload[strcspn(payload, "\r\n")] = '\0';

    md3_key_t key;
    md3_press_t press = PRESS_CLICK;
    char *separator = strchr(payload, ':');
    if (separator) {
        *separator++ = '\0';
    }
    const bool valid_key = !strcmp(payload, "vol_up") || !strcmp(payload, "vol_down") ||
                           !strcmp(payload, "menu") || !strcmp(payload, "vista");
    if (!valid_key) {
        ESP_LOGW(TAG, "ignoring invalid key request");
        return;
    }
    if (!strcmp(payload, "vol_up")) {
        key = KEY_VOL_UP;
    } else if (!strcmp(payload, "vol_down")) {
        key = KEY_VOL_DOWN;
    } else if (!strcmp(payload, "menu")) {
        key = KEY_MENU;
    } else {
        key = KEY_VISTA;
    }
    if (separator && strcmp(separator, "click")) {
        if (!strcmp(separator, "double")) {
            press = PRESS_DOUBLE;
        } else if (!strcmp(separator, "long")) {
            press = PRESS_LONG;
        } else if (!strcmp(separator, "hold")) {
            press = PRESS_HOLD;
        } else {
            ESP_LOGW(TAG, "ignoring invalid key press type");
            return;
        }
    }
    if (storage_remove(KEY_REQUEST_PATH) != ESP_OK) {
        return;
    }
    md3_event_t ev = {.type = EV_KEY, .key = key, .press = press};
    process_event(&ev);
    return_after_request();
}

// The host normally takes an idle volume back on its next TEST UNIT READY (usb_msc.c). A host
// that stopped polling after an eject gets it back through a reconnect instead.
#define HOST_POLL_GRACE_US (5 * 1000 * 1000)

static void return_idle_volume(void)
{
    if (!g_settings.udisk_mode || !tud_mounted() || tud_suspended() ||
        esp_timer_get_time() - usb_msc_last_poll_us() < HOST_POLL_GRACE_US) {
        return;
    }
    if (storage_release_to_host(HOST_POLL_GRACE_US) == ESP_OK) {
        ESP_LOGI(TAG, "host stopped polling; reconnecting to present the volume");
        usb_msc_reconnect();
    }
}

static void offer_volume(void)
{
    // An explicit host request: skip the idle wait.
    vTaskDelay(pdMS_TO_TICKS(100));  // let the host see the request's status stage before we drop off
    const esp_err_t result = storage_release_to_host(0);
    if (result == ESP_ERR_NOT_FOUND) {
        return;  // the host already has it; re-presenting would yank a mounted volume
    }
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "volume offer refused: held (0x%lx)", (unsigned long)storage_holds());
        return;
    }
    if (g_settings.udisk_mode && tud_mounted()) {
        usb_msc_reconnect();
    }
}

static void poll_link(void)
{
    account_power();  // the stream share changes with g_link
    if (link_poll(&g_link)) {
        md3_event_t ev = {.type = EV_LINK};
        process_event(&ev);
    }
}

// The DAC automutes a few seconds into silence; after AMP_IDLE_US of that (or of no stream) the
// headphone amps power down, and they come back as soon as the automute clears. A DAC power cycle turns them on itself.
static void amp_idle_tick(void)
{
    account_power();
    if (dac_reset_count() != s_amp_resets) {
        s_amp_resets = dac_reset_count();
        s_amp_off = false;
        s_muted_since = 0;
    }
    if (g_settings.udisk_mode || !dac_present()) {
        s_dac_muted = true;  // no audio path
        return;
    }
    // No stream at all counts as silence too: without clocks the automute flag may never rise.
    const bool muted = !g_link.rate_hz || dac_automuted();
    s_dac_muted = muted;
    const int64_t now = esp_timer_get_time();
    if (!muted) {
        s_muted_since = 0;
        if (s_amp_off) {
            board_amp_set(true);
            s_amp_off = false;
            ESP_LOGI(TAG, "amps on");
        }
    } else if (!s_muted_since) {
        s_muted_since = now;
    } else if (!s_amp_off && now - s_muted_since >= AMP_IDLE_US) {
        board_amp_set(false);
        s_amp_off = true;
        ESP_LOGI(TAG, "amps off after %lld s of automute", AMP_IDLE_US / 1000000);
    }
}

static void housekeeping(void)
{
    // Every local mount (boot, host eject, DAC mode) holds the volume until the
    // pending host requests have been checked; only then may the host take it back.
    if (storage_holds() & STORAGE_HOLD_REQUESTS) {
        const uint32_t generation = storage_mount_generation();
        if (storage_mounted()) {
            check_rom_request();
            check_screenshot_request();
            check_key_request();
        }
        storage_requests_checked(generation);  // a mount made meanwhile keeps its hold
    }
    return_idle_volume();
    poll_link();  // fallback; the link's ISR normally wakes the task at once
    // A charge-state change only moves the "usb src" line (power_tick() does that). The stock
    // firmware also reran the audio power-on here, which reset the Savitech and made the phone
    // drop the DAC whenever a charger went into the pass-through port.
    (void)power_tick();
    eq_apply();  // cheap when nothing changed; reloads after any DAC reset (power cycle, mode switch)
    amp_idle_tick();
    if (s_awake && lvgl_port_lock(0)) {
        if (esp_timer_get_time() - s_last_key_us >= AWAKE_US) {
            screen_sleep();
        } else {
            refresh_power();  // only redraws when the text changes
        }
        lvgl_port_unlock();
    }
    settings_tick();
    publish_state();
}

// Runs in the I2C-slave ISR after each write from the Savitech: queue one EV_LINK wake-up, not one
// per register, so a burst of writes cannot crowd keys out of the queue.
static bool link_wake_from_isr(void)
{
    if (s_link_signalled || !s_queue) {
        return false;
    }
    s_link_signalled = true;
    const md3_event_t ev = {.type = EV_LINK};
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &ev, &woken) != pdTRUE) {
        s_link_signalled = false;  // queue full; the next write or housekeeping catches up
    }
    return woken == pdTRUE;
}

// Event-driven: the task sleeps until a key, a USB request or a link write arrives, and otherwise
// wakes only for housekeeping every HOUSEKEEPING_MS (it used to poll everything every 40 ms).
static void ui_task(void *arg)
{
    TickType_t last_housekeeping = xTaskGetTickCount();
    for (;;) {
        const TickType_t period = pdMS_TO_TICKS(HOUSEKEEPING_MS);
        const TickType_t elapsed = xTaskGetTickCount() - last_housekeeping;
        TickType_t wait = elapsed < period ? period - elapsed : 0;
        if (s_amp_off && wait > pdMS_TO_TICKS(AMP_POLL_MS)) {
            wait = pdMS_TO_TICKS(AMP_POLL_MS);
        }
        md3_event_t ev = {0};
        if (xQueueReceive(s_queue, &ev, wait) == pdTRUE) {
            if (ev.type == EV_LINK) {
                s_link_signalled = false;  // cleared before reading, so a write during the read re-signals
                poll_link();
            } else if (ev.type == EV_OFFER) {
                offer_volume();
            } else if (ev.type == EV_ROM) {
                reboot_to_rom();
            } else {
                process_event(&ev);
            }
        }
        if (xTaskGetTickCount() - last_housekeeping >= period) {
            last_housekeeping = xTaskGetTickCount();
            housekeeping();
        } else if (s_amp_off) {
            amp_idle_tick();
        }
    }
}

// ------------------------------------------------------------------ public

esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    s_io = io;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen", &s_cpu_lock) != ESP_OK) {
        s_cpu_lock = NULL;  // power management off: nothing to hold
    }
    s_curve_points = calloc(eq_curve_points(), sizeof(*s_curve_points));
    ESP_RETURN_ON_FALSE(s_curve_points, ESP_ERR_NO_MEM, TAG, "curve points");
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl port");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 40,
        .double_buffer = true,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {.buff_dma = true, .swap_bytes = true, .sw_rotate = true},
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display");

    lvgl_port_lock(0);
    build_screen();
    apply_rotation();
    // Start dark: board_lcd_init() left the panel awake with the backlight at 0.
    account_power();  // the time before this counts as dark
    s_awake = true;
    s_panel_toggle_us = esp_timer_get_time();
    screen_sleep();
    lvgl_port_unlock();
    publish_state();
    return ESP_OK;
}

esp_err_t ui_start(void)
{
    s_queue = xQueueCreate(16, sizeof(md3_event_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    link_set_notify(link_wake_from_isr);
    return xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

const char *ui_screen_name(void)
{
    return s_awake ? "status" : "off";
}

esp_err_t ui_screenshot_bmp(uint8_t **out, size_t *len)
{
    *out = NULL;
    *len = 0;
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    lvgl_port_unlock();
    ESP_RETURN_ON_FALSE(snap, ESP_ERR_NO_MEM, TAG, "snapshot");

    const int w = snap->header.w, h = snap->header.h;
    const int row = (w * 3 + 3) & ~3;
    const size_t size = 54 + (size_t)row * h;
    uint8_t *bmp = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        lv_draw_buf_destroy(snap);
        return ESP_ERR_NO_MEM;
    }
    const uint32_t hdr[] = {size, 0, 54, 40, (uint32_t)w, (uint32_t)h, 1 | (24 << 16), 0, row * h, 2835, 2835, 0, 0};
    bmp[0] = 'B';
    bmp[1] = 'M';
    memcpy(bmp + 2, hdr, sizeof(hdr));
    for (int y = 0; y < h; y++) {
        // LVGL RGB888 is stored B,G,R, which is BMP's order; BMP rows run bottom-up.
        memcpy(bmp + 54 + (size_t)(h - 1 - y) * row, snap->data + (size_t)y * snap->header.stride, w * 3);
    }
    lv_draw_buf_destroy(snap);
    *out = bmp;
    *len = size;
    return ESP_OK;
}
