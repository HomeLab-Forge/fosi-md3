// Shared types for the MD3 firmware: UI events and coherent read-only status snapshots.
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
    EV_CLOCK,         // value = seconds of day, set by the host
    EV_SUN,           // value = sunrise minute | sunset minute << 16, set by the host
    EV_OFFER,         // host asks for the volume back now
    EV_ROM,           // host asks for ROM download mode
} md3_event_type_t;

typedef struct {
    md3_event_type_t type;
    md3_key_t key;
    md3_press_t press;
    uint32_t value;   // EV_CLOCK, EV_SUN argument
} md3_event_t;

/** Queue an event for the UI task; false if the queue is absent or full. */
bool md3_post(const md3_event_t *ev);

/** A coherent status copy. The UI task publishes; USB control readers never touch live backends. */
typedef struct {
    md3_settings_t settings;
    link_state_t link;
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
