// Shared types for the MD3 firmware: UI events, remote payloads, settings patches, and
// coherent read-only status snapshots.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "link.h"
#include "settings.h"

#define MD3_FW_VERSION "md3fw 0.2.4"

typedef enum {
    KEY_VOL_UP,
    KEY_VOL_DOWN,
    KEY_MENU,
    KEY_VISTA,
} md3_key_t;

typedef enum {
    PRESS_CLICK,
    PRESS_DOUBLE,
    PRESS_LONG,
    PRESS_HOLD,  // repeating while held (volume keys)
} md3_press_t;

typedef enum {
    EV_KEY,
    EV_LINK,          // host volume / rate changed
    EV_SETTINGS,      // data = md3_settings_patch_t (ownership passes to UI)
    EV_SHOW_FRAME,    // data = RGB565 240x240 in PSRAM (ownership passes to UI)
    EV_SHOW_GIF,      // data = GIF bytes in PSRAM (ownership passes to UI)
    EV_SHOW_JPEG,     // data = JPEG bytes in PSRAM (ownership passes to UI)
    EV_SHOW_TEXT,     // data = NUL-terminated string (ownership passes to UI)
    EV_HOME,
    EV_NET,           // Wi-Fi state changed
    EV_CLOCK,         // value = seconds of day, set by the host
    EV_SUN,           // value = sunrise minute | sunset minute << 16, set by the host
    EV_OFFER,         // host asks for the volume back now
    EV_ROM,           // host asks for ROM download mode
} md3_event_type_t;

typedef struct {
    md3_event_type_t type;
    md3_key_t key;
    md3_press_t press;
    uint8_t *data;
    size_t len;
    uint32_t color;   // EV_SHOW_TEXT foreground
    uint32_t value;   // EV_CLOCK, EV_SUN argument
} md3_event_t;

/** Queue an event for the UI task; frees data if the queue is absent or full. */
bool md3_post(const md3_event_t *ev);

/** Network state. Access through md3_net_get/set, never by unlocked struct reads. */
typedef struct {
    bool connected;
    char ip[16];      // IPv4; empty while disconnected
    int rssi;
    bool scan_done;
    int scan_error;   // esp_err_t from the boot radio probe; zero on success
    uint16_t scan_count;
    int scan_rssi;    // strongest beacon found by the boot probe
} md3_net_t;

extern md3_net_t g_net;

typedef enum {
    MD3_SET_VOLUME = 1U << 0,
    MD3_SET_FILTER = 1U << 1,
    MD3_SET_GAIN_HIGH = 1U << 2,
    MD3_SET_VOL_FROM_HOST = 1U << 3,
    MD3_SET_BRIGHTNESS = 1U << 4,
    MD3_SET_ROTATION = 1U << 5,
    MD3_SET_UAC1 = 1U << 6,
} md3_setting_mask_t;

typedef struct {
    uint32_t mask;
    md3_settings_t value;
} md3_settings_patch_t;

/** A coherent status copy. Producers publish; HTTP readers never touch live backends. */
typedef struct {
    md3_settings_t settings;
    link_state_t link;
    md3_net_t net;
    bool dac_present;
    int power_vbus_mv;
    bool power_charging;
    bool storage_mounted;
    uint64_t storage_total;
    uint64_t storage_free;
    uint32_t storage_holds;
    char screen[8];
    bool clock_set;   // false until a host sets the time; the day-wheel then shows no time
    int clock_seconds;
    bool sun_known;   // false: the wheel splits day and night at 06:00 and 18:00
    int sun_rise;     // minutes of day
    int sun_set;
} md3_snapshot_t;

void md3_state_init(void);
void md3_state_publish(const md3_snapshot_t *snapshot);
void md3_state_read(md3_snapshot_t *out);
void md3_net_set(const md3_net_t *state);
void md3_net_get(md3_net_t *out);
