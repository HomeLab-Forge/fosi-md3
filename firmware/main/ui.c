#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "board.h"
#include "dac.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "link.h"
#include "md3.h"
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "tinyusb.h"
#include "usb_msc.h"

static const char *TAG = "ui";

#define FRAME_BYTES     (LCD_H_RES * LCD_V_RES * 2)
#define COLOR_BG        lv_color_hex(0x06080A)
#define COLOR_TRACK     lv_color_hex(0x202428)
#define COLOR_ACCENT    lv_color_hex(0xFF6018)
#define COLOR_TEXT      lv_color_hex(0xF0F0F0)
#define COLOR_DIM       lv_color_hex(0x8A8F94)
#define HOLD_REPEAT_US  70000
#define TOAST_MS        1500

typedef enum { SCR_MAIN, SCR_ALBUM, SCR_REMOTE, SCR_MENU, SCR_CLOCK } screen_t;

extern link_state_t g_link;

static QueueHandle_t s_queue;
static lv_display_t *s_disp;
static screen_t s_screen = SCR_MAIN;
static bool s_vista_locked;
static bool s_backlight_off;
static int64_t s_last_hold_us;

// main
static lv_obj_t *s_main, *s_arc, *s_vol, *s_rate, *s_status;
// album / remote share one full-screen stage
static lv_obj_t *s_stage, *s_stage_img, *s_stage_gif, *s_stage_text;
static lv_image_dsc_t s_stage_dsc;
static uint8_t *s_stage_pixels;   // RGB565 frame shown by s_stage_img
static uint8_t *s_stage_gif_data; // GIF bytes kept alive while playing
static lv_image_dsc_t s_gif_dsc;
// album position
static int s_album_index;
// menu
static lv_obj_t *s_menu, *s_menu_title, *s_menu_rows[5];
static int s_menu_sel;
// volume toast on the top layer
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
// 24-hour day-wheel
static lv_obj_t *s_clock, *s_clock_hand, *s_clock_tip, *s_clock_time, *s_clock_day, *s_clock_night;
static lv_point_precise_t s_clock_points[2];
static int s_clock_last_minute = -1;
static int s_clock_seconds;        // seconds of day at s_clock_epoch_us
static int64_t s_clock_epoch_us;
static bool s_clock_set;           // only a host knows the time; until then the wheel shows none
static int s_sun_rise = 6 * 60;    // minutes of day; 06:00/18:00 until a host sends the real sun
static int s_sun_set = 18 * 60;
static bool s_sun_known;
static uint64_t s_storage_total;
static uint64_t s_storage_free;
static int64_t s_storage_read_us;

bool md3_post(const md3_event_t *ev)
{
    return s_queue && xQueueSend(s_queue, ev, 0) == pdTRUE;
}

// ------------------------------------------------------------------ helpers

static const int k_brightness_pct[5] = {20, 40, 60, 80, 100};

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
        .dac_present = dac_present(),
        .power_vbus_mv = power_vbus_mv(),
        .power_charging = power_charging(),
        .storage_mounted = storage_mounted(),
        .storage_total = s_storage_total,
        .storage_free = s_storage_free,
        .storage_holds = storage_holds(),
        .clock_set = s_clock_set,
        .clock_seconds = (int)((s_clock_seconds + (esp_timer_get_time() - s_clock_epoch_us) / 1000000) % 86400),
        .sun_known = s_sun_known,
        .sun_rise = s_sun_rise,
        .sun_set = s_sun_set,
    };
    strlcpy(snapshot.screen, ui_screen_name(), sizeof(snapshot.screen));
    md3_state_publish(&snapshot);
}

static void apply_brightness(void)
{
    board_backlight_set(s_backlight_off ? 0 : k_brightness_pct[g_settings.brightness % 5]);
}

static void apply_rotation(void)
{
    static const lv_display_rotation_t rot[4] = {
        LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90, LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270,
    };
    // Panel MY handles the hardware reflection; the saved user rotation applies directly.
    lv_display_set_rotation(s_disp, rot[g_settings.rotation & 3]);
}

static const char *filter_name(int f)
{
    static const char *names[8] = {
        "Min phase", "Lin fast apod", "Lin fast", "Lin fast LR",
        "Lin slow", "Min fast", "Min slow", "Min slow LD",
    };
    return names[f & 7];
}

static void stage_clear(void)
{
    lv_obj_add_flag(s_stage_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_stage_text, LV_OBJ_FLAG_HIDDEN);
    if (s_stage_gif) {
        lv_obj_delete(s_stage_gif);
        s_stage_gif = NULL;
    }
    free(s_stage_gif_data);
    s_stage_gif_data = NULL;
}

static void show_screen(screen_t scr)
{
    s_screen = scr;
    storage_hold(STORAGE_HOLD_ALBUM, scr == SCR_ALBUM);
    lv_obj_t *target = scr == SCR_MAIN ? s_main : scr == SCR_MENU ? s_menu : scr == SCR_CLOCK ? s_clock : s_stage;
    lv_screen_load(target);
}

// ------------------------------------------------------------------ main dial

static void main_refresh(void)
{
    char buf[48];
    lv_arc_set_value(s_arc, g_settings.volume);
    if (g_settings.vol_from_host && g_link.host_mute) {
        lv_label_set_text(s_vol, "MUTE");
    } else {
        lv_label_set_text_fmt(s_vol, "%u", g_settings.volume);
    }
    lv_label_set_text(s_rate, g_link.rate_label);
    snprintf(buf, sizeof(buf), "F%d  %s  %s%s", g_settings.filter + 1, g_settings.gain_high ? "HI" : "LO",
             g_settings.vol_from_host ? "HOST" : "FIXED", s_vista_locked ? "  " LV_SYMBOL_EYE_CLOSE : "");
    lv_label_set_text(s_status, buf);
}

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_main, COLOR_BG, 0);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    s_arc = lv_arc_create(s_main);
    lv_obj_set_size(s_arc, 228, 228);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, COLOR_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_ACCENT, LV_PART_INDICATOR);

    s_vol = lv_label_create(s_main);
    lv_obj_set_style_text_font(s_vol, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_vol, COLOR_TEXT, 0);
    lv_obj_align(s_vol, LV_ALIGN_CENTER, 0, -8);

    s_rate = lv_label_create(s_main);
    lv_obj_set_style_text_font(s_rate, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_rate, COLOR_ACCENT, 0);
    lv_obj_align(s_rate, LV_ALIGN_CENTER, 0, 36);

    s_status = lv_label_create(s_main);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status, COLOR_DIM, 0);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 68);
    main_refresh();
}

// ------------------------------------------------------------------ 24-hour day-wheel

static void clock_refresh(void)
{
    const int64_t elapsed = (esp_timer_get_time() - s_clock_epoch_us) / 1000000;
    const int minute = (int)((s_clock_seconds + elapsed) % 86400) / 60;
    if (minute == s_clock_last_minute) {
        return;
    }
    s_clock_last_minute = minute;
    if (!s_clock_set) {
        // No host has told us the time since boot: show no hand rather than a guess.
        lv_obj_add_flag(s_clock_hand, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_clock_tip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_clock_time, COLOR_DIM, 0);
        lv_label_set_text(s_clock_time, "--:--");
        return;
    }
    lv_obj_remove_flag(s_clock_hand, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_clock_tip, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(s_clock_time, COLOR_ACCENT, 0);
    const float angle = (minute - 12 * 60) * (float)M_PI / (12 * 60);
    const float x = 120 + 82 * sinf(angle);
    const float y = 120 - 82 * cosf(angle);
    s_clock_points[0] = (lv_point_precise_t){120, 120};
    s_clock_points[1] = (lv_point_precise_t){x, y};
    lv_line_set_points(s_clock_hand, s_clock_points, 2);
    lv_obj_set_pos(s_clock_tip, (int)x - 7, (int)y - 7);
    const bool daylight = s_sun_rise < s_sun_set ? minute >= s_sun_rise && minute < s_sun_set
                                                 : minute >= s_sun_rise || minute < s_sun_set;
    const lv_color_t hand_color = daylight ? COLOR_ACCENT : lv_color_hex(0x80B0D0);
    lv_obj_set_style_line_color(s_clock_hand, hand_color, 0);
    lv_obj_set_style_bg_color(s_clock_tip, hand_color, 0);
    lv_label_set_text_fmt(s_clock_time, "%02d:%02d", minute / 60, minute % 60);
}

static void clock_set_seconds(int seconds)
{
    s_clock_seconds = seconds % 86400;
    s_clock_epoch_us = esp_timer_get_time();
    s_clock_set = true;
    s_clock_last_minute = -1;
    clock_refresh();
}

// LVGL arc angle (0 = 3 o'clock, clockwise) of a minute of day on the wheel (noon up, midnight down).
static int wheel_angle(int minute)
{
    return ((minute + 12 * 60) % (24 * 60) / 4 + 270) % 360;
}

static void clock_apply_sun(void)
{
    lv_arc_set_bg_angles(s_clock_day, wheel_angle(s_sun_rise), wheel_angle(s_sun_set));
    lv_arc_set_bg_angles(s_clock_night, wheel_angle(s_sun_set), wheel_angle(s_sun_rise));
    s_clock_last_minute = -1;
    clock_refresh();
}

static void build_clock(void)
{
    s_clock = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_clock, COLOR_BG, 0);
    lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *day = s_clock_day = lv_arc_create(s_clock);
    lv_obj_set_size(day, 208, 208);
    lv_obj_center(day);
    lv_arc_set_bg_angles(day, 180, 360);  // 06:00 left -> noon -> 18:00 right, upper half
    lv_arc_set_range(day, 0, 100);
    lv_arc_set_value(day, 100);
    lv_obj_remove_style(day, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(day, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(day, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(day, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(day, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(day, lv_color_hex(0x705020), LV_PART_INDICATOR);

    lv_obj_t *night = s_clock_night = lv_arc_create(s_clock);
    lv_obj_set_size(night, 208, 208);
    lv_obj_center(night);
    lv_arc_set_bg_angles(night, 0, 180);  // 18:00 right -> midnight -> 06:00 left, lower half
    lv_arc_set_range(night, 0, 100);
    lv_arc_set_value(night, 100);
    lv_obj_remove_style(night, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(night, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(night, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(night, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(night, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(night, lv_color_hex(0x203848), LV_PART_INDICATOR);

    const struct { const char *text; int x; int y; } marks[] = {
        {"12", 0, -96}, {"18", 96, 0}, {"00", 0, 96}, {"06", -96, 0},
    };
    for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); i++) {
        lv_obj_t *mark = lv_label_create(s_clock);
        lv_label_set_text(mark, marks[i].text);
        lv_obj_set_style_text_font(mark, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(mark, COLOR_DIM, 0);
        lv_obj_align(mark, LV_ALIGN_CENTER, marks[i].x, marks[i].y);
    }
    lv_obj_t *title = lv_label_create(s_clock);
    lv_label_set_text(title, "DAY WHEEL");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, COLOR_DIM, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -52);

    s_clock_hand = lv_line_create(s_clock);
    lv_obj_set_style_line_width(s_clock_hand, 4, 0);
    lv_obj_set_style_line_color(s_clock_hand, COLOR_ACCENT, 0);
    lv_obj_set_style_line_rounded(s_clock_hand, true, 0);

    s_clock_tip = lv_obj_create(s_clock);
    lv_obj_set_size(s_clock_tip, 14, 14);
    lv_obj_set_style_radius(s_clock_tip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_clock_tip, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_clock_tip, 0, 0);
    lv_obj_remove_flag(s_clock_tip, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hub = lv_obj_create(s_clock);
    lv_obj_set_size(hub, 8, 8);
    lv_obj_center(hub);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(hub, COLOR_TEXT, 0);
    lv_obj_set_style_border_width(hub, 0, 0);
    lv_obj_remove_flag(hub, LV_OBJ_FLAG_CLICKABLE);

    s_clock_time = lv_label_create(s_clock);
    lv_obj_set_style_text_font(s_clock_time, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_clock_time, COLOR_ACCENT, 0);
    lv_obj_align(s_clock_time, LV_ALIGN_CENTER, 0, 52);
    s_clock_epoch_us = esp_timer_get_time();
    clock_apply_sun();
}

// ------------------------------------------------------------------ stage (album + remote)

static bool build_stage(void)
{
    s_stage = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_stage, lv_color_black(), 0);
    lv_obj_remove_flag(s_stage, LV_OBJ_FLAG_SCROLLABLE);

    s_stage_pixels = heap_caps_calloc(1, FRAME_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_stage_pixels) {
        return false;
    }
    s_stage_dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = LCD_H_RES, .h = LCD_V_RES, .stride = LCD_H_RES * 2},
        .data_size = FRAME_BYTES,
        .data = s_stage_pixels,
    };
    s_stage_img = lv_image_create(s_stage);
    lv_obj_center(s_stage_img);
    lv_obj_add_flag(s_stage_img, LV_OBJ_FLAG_HIDDEN);

    s_stage_text = lv_label_create(s_stage);
    lv_label_set_long_mode(s_stage_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_stage_text, 180);
    lv_obj_set_style_text_align(s_stage_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_stage_text, &lv_font_montserrat_28, 0);
    lv_obj_center(s_stage_text);
    lv_obj_add_flag(s_stage_text, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static void stage_show_pixels(int w, int h)
{
    s_stage_dsc.header.w = w;
    s_stage_dsc.header.h = h;
    s_stage_dsc.header.stride = w * 2;
    s_stage_dsc.data_size = w * h * 2;
    lv_image_cache_drop(&s_stage_dsc);
    lv_image_set_src(s_stage_img, &s_stage_dsc);
    lv_obj_center(s_stage_img);
    lv_obj_remove_flag(s_stage_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_stage_img);
}

static esp_err_t stage_show_jpeg(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    ESP_RETURN_ON_ERROR(esp_jpeg_get_image_info(&cfg, &info), TAG, "jpeg header");
    for (esp_jpeg_image_scale_t s = JPEG_IMAGE_SCALE_0; info.width > LCD_H_RES || info.height > LCD_V_RES; ) {
        ESP_RETURN_ON_FALSE(s < JPEG_IMAGE_SCALE_1_8, ESP_ERR_INVALID_SIZE, TAG, "jpeg too large");
        s++;
        cfg.out_scale = s;
        ESP_RETURN_ON_ERROR(esp_jpeg_get_image_info(&cfg, &info), TAG, "jpeg scale");
    }
    cfg.outbuf = s_stage_pixels;
    cfg.outbuf_size = FRAME_BYTES;
    ESP_RETURN_ON_ERROR(esp_jpeg_decode(&cfg, &info), TAG, "jpeg decode");
    stage_show_pixels(info.width, info.height);
    return ESP_OK;
}

static bool stage_show_gif(uint8_t *data, size_t len)
{
    s_stage_gif_data = data;  // owned until the next stage_clear()
    // LVGL 9.2's decoder accepts only GIF89a, while much of the stock library is
    // GIF87a. The basic frame format is compatible; normalize only our RAM copy.
    if (len >= 6 && !memcmp(data, "GIF87a", 6)) {
        memcpy(data + 3, "89a", 3);
    }
    s_gif_dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RAW},
        .data_size = len,
        .data = data,
    };
    s_stage_gif = lv_gif_create(s_stage);
    lv_gif_set_src(s_stage_gif, &s_gif_dsc);
    if (!lv_gif_is_loaded(s_stage_gif)) {
        lv_obj_delete(s_stage_gif);
        s_stage_gif = NULL;
        free(s_stage_gif_data);
        s_stage_gif_data = NULL;
        return false;
    }
    lv_obj_center(s_stage_gif);
    return true;
}

static void stage_show_text(const char *text, uint32_t color)
{
    lv_label_set_text(s_stage_text, text);
    lv_obj_set_style_text_color(s_stage_text, lv_color_hex(color), 0);
    lv_obj_remove_flag(s_stage_text, LV_OBJ_FLAG_HIDDEN);
}

// ------------------------------------------------------------------ album

static void album_show(int step_folder, int step_file)
{
    // Opening the album is an explicit request for local ownership. Do not rely on
    // every USB host sending the same eject/detach sequence before allowing reads.
    if (storage_mount() != ESP_OK) {
        stage_clear();
        show_screen(SCR_ALBUM);
        stage_show_text("Storage unavailable", 0x8A8F94);
        return;
    }
    int folder = (g_settings.album_folder + step_folder) % STORAGE_FOLDERS;
    char **names = NULL;
    int count = 0;
    // Skip empty folders, at most one full lap.
    for (int lap = 0; lap < STORAGE_FOLDERS; lap++) {
        count = storage_list(folder, &names);
        if (count > 0) {
            break;
        }
        storage_free_list(names, count);
        names = NULL;
        folder = (folder + 1) % STORAGE_FOLDERS;
    }
    stage_clear();
    show_screen(SCR_ALBUM);
    if (count == 0) {
        stage_show_text("No pictures", 0x8A8F94);
        return;
    }
    if (folder != g_settings.album_folder) {
        g_settings.album_folder = folder;
        settings_touch();
        s_album_index = 0;
    }
    if (folder >= 3) {
        s_album_index = esp_random() % count;  // games draw at random, like the stock firmware
    } else {
        s_album_index = (s_album_index + step_file + count) % count;
    }
    char path[96];
    snprintf(path, sizeof(path), STORAGE_ROOT "/%s/%s", k_storage_folders[folder], names[s_album_index]);
    storage_free_list(names, count);

    uint8_t *data = NULL;
    size_t len = 0;
    if (storage_read(path, &data, &len) != ESP_OK) {
        stage_show_text("Can't open picture", 0x8A8F94);
        return;
    }
    ESP_LOGI(TAG, "album %s (%u bytes)", path, (unsigned)len);
    if (len > 6 && !memcmp(data, "GIF8", 4)) {
        if (!stage_show_gif(data, len)) {
            stage_show_text("Unsupported picture", 0x8A8F94);
        }
    } else {
        if (stage_show_jpeg(data, len) != ESP_OK) {
            stage_show_text("Unsupported picture", 0x8A8F94);
        }
        free(data);
    }
}

// ------------------------------------------------------------------ menu

typedef enum {
    MI_FILTER, MI_GAIN, MI_HOSTVOL, MI_BRIGHT, MI_ROTATE, MI_UAC, MI_UDISK, MI_CLOCK, MI_EXIT, MI_COUNT,
} menu_item_t;

static void menu_item_text(int item, char *buf, size_t n)
{
    switch (item) {
    case MI_FILTER: snprintf(buf, n, "Filter: %s", filter_name(g_settings.filter)); break;
    case MI_GAIN: snprintf(buf, n, "Gain: %s", g_settings.gain_high ? "High" : "Low"); break;
    case MI_HOSTVOL: snprintf(buf, n, "Host volume: %s", g_settings.vol_from_host ? "On" : "Off"); break;
    case MI_BRIGHT: snprintf(buf, n, "Brightness: %d%%", k_brightness_pct[g_settings.brightness % 5]); break;
    case MI_ROTATE: snprintf(buf, n, "Rotate: %d", (g_settings.rotation & 3) * 90); break;
    case MI_UAC: snprintf(buf, n, "USB audio: UAC %s", g_settings.uac1 ? "1.0" : "2.0"); break;
    case MI_UDISK: snprintf(buf, n, "USB mode: %s", g_settings.udisk_mode ? "U-Disk" : "DAC"); break;
    case MI_CLOCK: snprintf(buf, n, "Day wheel"); break;
    default: snprintf(buf, n, "Exit"); break;
    }
}

static void menu_refresh(void)
{
    char buf[40];
    for (int row = 0; row < 5; row++) {
        const int item = s_menu_sel + row - 2;
        lv_obj_t *label = s_menu_rows[row];
        if (item < 0 || item >= MI_COUNT) {
            lv_label_set_text(label, "");
            continue;
        }
        menu_item_text(item, buf, sizeof(buf));
        lv_label_set_text(label, buf);
        const bool sel = row == 2;
        lv_obj_set_style_text_color(label, sel ? COLOR_BG : (row == 1 || row == 3 ? COLOR_TEXT : COLOR_DIM), 0);
        lv_obj_set_style_bg_opa(label, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void build_menu(void)
{
    s_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_menu, COLOR_BG, 0);
    lv_obj_remove_flag(s_menu, LV_OBJ_FLAG_SCROLLABLE);
    s_menu_title = lv_label_create(s_menu);
    lv_label_set_text(s_menu_title, "SETTINGS");
    lv_obj_set_style_text_color(s_menu_title, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_menu_title, &lv_font_montserrat_14, 0);
    lv_obj_align(s_menu_title, LV_ALIGN_TOP_MID, 0, 26);
    for (int row = 0; row < 5; row++) {
        lv_obj_t *l = lv_label_create(s_menu);
        lv_obj_set_width(l, row == 2 ? 200 : 180);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_bg_color(l, COLOR_ACCENT, 0);
        lv_obj_set_style_radius(l, 6, 0);
        lv_obj_set_style_pad_ver(l, 4, 0);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, (row - 2) * 34);
        s_menu_rows[row] = l;
    }
}

static void menu_activate(void)
{
    bool audio_changed = false;
    bool power_cycle = false;
    switch (s_menu_sel) {
    case MI_FILTER: g_settings.filter = (g_settings.filter + 1) & 7; dac_apply_filter(); break;
    case MI_GAIN: g_settings.gain_high ^= 1; audio_changed = true; break;
    case MI_HOSTVOL: g_settings.vol_from_host ^= 1; audio_changed = true; break;
    case MI_BRIGHT: g_settings.brightness = (g_settings.brightness + 1) % 5; apply_brightness(); break;
    case MI_ROTATE: g_settings.rotation = (g_settings.rotation + 1) & 3; apply_rotation(); break;
    case MI_UAC:
        g_settings.uac1 ^= 1;
        power_cycle = true;
        break;
    case MI_UDISK:
        g_settings.udisk_mode ^= 1;
        power_cycle = true;
        break;
    case MI_CLOCK: show_screen(SCR_CLOCK); return;
    default: main_refresh(); show_screen(SCR_MAIN); return;
    }
    if (power_cycle) {
        if (audio_power_cycle() != ESP_OK) {
            ESP_LOGW(TAG, "DAC not ready after UAC change");
        }
    } else if (audio_changed) {
        dac_apply_volume();
    }
    settings_touch();
    menu_refresh();
}

// ------------------------------------------------------------------ volume toast

static void toast_hide(lv_timer_t *t)
{
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}

static void toast_show(void)
{
    if (s_screen == SCR_MAIN) {
        return;
    }
    lv_label_set_text_fmt(s_toast, "VOL %u", g_settings.volume);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}

static void build_toast(void)
{
    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_toast, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
    lv_obj_set_style_text_color(s_toast, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_20, 0);
    lv_obj_set_style_pad_all(s_toast, 6, 0);
    lv_obj_set_style_radius(s_toast, 8, 0);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = lv_timer_create(toast_hide, TOAST_MS, NULL);
    lv_timer_pause(s_toast_timer);
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
    main_refresh();
    toast_show();
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
    if (s_backlight_off) {  // any key wakes the screen and is otherwise consumed
        s_backlight_off = false;
        apply_brightness();
        return;
    }
    const bool vol = key == KEY_VOL_UP || key == KEY_VOL_DOWN;
    // Menus scroll with the volume keys; at 180/270 degrees the stock firmware swaps them.
    const bool flipped = (g_settings.rotation & 3) >= 2;
    const int dir = (key == KEY_VOL_UP ? 1 : -1) * (flipped && s_screen == SCR_MENU ? -1 : 1);

    switch (s_screen) {
    case SCR_MENU:
        if (vol && press != PRESS_LONG) {
            s_menu_sel = (s_menu_sel - dir + MI_COUNT) % MI_COUNT;
            menu_refresh();
        } else if (key == KEY_MENU && press == PRESS_CLICK) {
            menu_activate();
        } else if (key == KEY_VISTA || (key == KEY_MENU && press == PRESS_LONG)) {
            main_refresh();
            show_screen(SCR_MAIN);
        }
        return;
    default:
        break;
    }

    if (s_screen == SCR_CLOCK) {
        main_refresh();
        show_screen(SCR_MAIN);
        return;
    }

    if (vol && press != PRESS_LONG) {
        volume_step(dir);
        return;
    }
    if (key == KEY_MENU) {
        if (press == PRESS_LONG) {
            s_vista_locked = !s_vista_locked;
            main_refresh();
        } else if (s_screen == SCR_MAIN) {
            s_menu_sel = 0;
            menu_refresh();
            show_screen(SCR_MENU);
        } else {
            stage_clear();
            main_refresh();
            show_screen(SCR_MAIN);
        }
        return;
    }
    if (key == KEY_VISTA && !s_vista_locked) {
        if (press == PRESS_LONG) {
            s_backlight_off = true;
            apply_brightness();
        } else if (press == PRESS_DOUBLE) {
            if (s_screen == SCR_ALBUM) {
                album_show(1, 0);
            } else if (s_screen == SCR_MAIN) {
                g_settings.rotation = (g_settings.rotation + 1) & 3;
                apply_rotation();
                settings_touch();
            }
        } else if (s_screen == SCR_ALBUM) {
            album_show(0, 1);
        } else if (s_screen == SCR_REMOTE) {
            stage_clear();
            main_refresh();
            show_screen(SCR_MAIN);
        } else {
            album_show(0, 0);
        }
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
        main_refresh();
        break;
    case EV_CLOCK:
        clock_set_seconds((int)ev->value);
        ESP_LOGI(TAG, "clock set to %02lu:%02lu:%02lu over USB control", (unsigned long)ev->value / 3600,
                 (unsigned long)ev->value / 60 % 60, (unsigned long)ev->value % 60);
        break;
    case EV_SUN:
        s_sun_rise = (int)(ev->value & 0xFFFF);
        s_sun_set = (int)(ev->value >> 16);
        s_sun_known = true;
        clock_apply_sun();
        break;
    case EV_OFFER:
    case EV_ROM:
        break;  // handled by ui_task outside the LVGL lock
    }
    publish_state();
}

static void process_event(md3_event_t *ev)
{
    if (lvgl_port_lock(0)) {
        handle_event(ev);
        lvgl_port_unlock();
    }
}

// A host asks for ROM download mode by writing ROM_REQUEST_PATH, starting with the marker, to
// the Photo Sync volume and ejecting it. The file is removed before the reboot, so it fires once.
#define ROM_REQUEST_PATH   STORAGE_ROOT "/MD3ROM.TXT"
#define ROM_REQUEST_MARKER "md3-enter-rom"

// After a request the host sees its answer at once: detach and reconnect, unless the album is
// using the volume (TEST UNIT READY returns it when the album closes).
static void return_after_request(void)
{
    if ((storage_holds() & STORAGE_HOLD_ALBUM) || storage_unmount() != ESP_OK) {
        return;
    }
    usb_msc_reconnect();
}

static void reboot_to_rom(void)
{
    if (lvgl_port_lock(0)) {
        stage_clear();
        show_screen(SCR_REMOTE);
        stage_show_text("ROM download mode", 0x8A8F94);
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

#define TIME_REQUEST_PATH   STORAGE_ROOT "/MD3TIME.TXT"
#define TIME_REQUEST_MARKER "md3-time="

static void check_time_request(void)
{
    uint8_t *request = NULL;
    size_t request_len = 0;
    if (storage_read(TIME_REQUEST_PATH, &request, &request_len) != ESP_OK) {
        return;
    }
    const size_t marker_len = strlen(TIME_REQUEST_MARKER);
    const char *text = request_len >= marker_len ? (const char *)request + marker_len : NULL;
    bool valid = request_len >= marker_len + 5 && !memcmp(request, TIME_REQUEST_MARKER, marker_len);
    for (size_t i = 0; valid && i < 5; i++) {
        if (i == 2) {
            valid = text[i] == ':';
        } else {
            valid = text[i] >= '0' && text[i] <= '9';
        }
    }
    int minute = 0;
    if (valid) {
        const int hour = (text[0] - '0') * 10 + text[1] - '0';
        minute = (text[3] - '0') * 10 + text[4] - '0';
        valid = hour >= 0 && hour < 24 && minute >= 0 && minute < 60;
        minute += hour * 60;
    }
    free(request);
    if (!valid) {
        ESP_LOGW(TAG, "ignoring invalid time request");
        return;
    }
    if (storage_remove(TIME_REQUEST_PATH) != ESP_OK) {
        return;
    }
    if (lvgl_port_lock(0)) {
        clock_set_seconds(minute * 60);
        lvgl_port_unlock();
    }
    ESP_LOGI(TAG, "clock set to %02d:%02d over Photo Sync", minute / 60, minute % 60);
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
    // An explicit host request: skip the idle wait, but never take the volume from the album.
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

static void housekeeping(void)
{
    // Every local mount (boot, host eject, DAC mode, album open) holds the volume until the
    // pending host requests have been checked; only then may the host take it back.
    if (storage_holds() & STORAGE_HOLD_REQUESTS) {
        const uint32_t generation = storage_mount_generation();
        if (storage_mounted()) {
            check_rom_request();
            check_screenshot_request();
            check_time_request();
            check_key_request();
        }
        storage_requests_checked(generation);  // a mount made meanwhile keeps its hold
    }
    return_idle_volume();
    if (link_poll(&g_link)) {
        md3_event_t ev = {.type = EV_LINK};
        process_event(&ev);
    }
    if (power_tick()) {
        if (audio_power_cycle() != ESP_OK) {
            ESP_LOGW(TAG, "DAC not ready after power change");
        }
    }
    settings_tick();
    if (s_screen == SCR_CLOCK && lvgl_port_lock(0)) {
        clock_refresh();
        lvgl_port_unlock();
    }
    publish_state();
}

static void ui_task(void *arg)
{
    TickType_t last_housekeeping = xTaskGetTickCount();
    for (;;) {
        md3_event_t ev = {0};
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(40)) == pdTRUE) {
            if (ev.type == EV_OFFER) {
                offer_volume();
            } else if (ev.type == EV_ROM) {
                reboot_to_rom();
            } else {
                process_event(&ev);
            }
        }
        const TickType_t now = xTaskGetTickCount();
        if (now - last_housekeeping >= pdMS_TO_TICKS(40)) {
            last_housekeeping = now;
            housekeeping();
        }
    }
}

// ------------------------------------------------------------------ public

esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
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
    build_main();
    if (!build_stage()) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    build_menu();
    build_toast();
    build_clock();
    apply_rotation();
    show_screen(SCR_CLOCK);
    lvgl_port_unlock();
    apply_brightness();
    publish_state();
    return ESP_OK;
}

esp_err_t ui_start(void)
{
    s_queue = xQueueCreate(16, sizeof(md3_event_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    return xTaskCreate(ui_task, "ui", 8192, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_FAIL;
}

const char *ui_screen_name(void)
{
    static const char *names[] = {"main", "album", "remote", "menu", "clock"};
    return names[s_screen];
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
