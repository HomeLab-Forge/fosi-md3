// Shared types for the MD3 firmware: UI events and coherent read-only status snapshots.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "eq.h"
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
    EV_OFFER,         // host asks for the volume back now
    EV_EQ_SCALE,      // value = log2_2x | log2_4x << 8 | (uint16_t)trim_tenth_db << 16
    EV_ROM,           // host asks for ROM download mode
} md3_event_type_t;

typedef struct {
    md3_event_type_t type;
    md3_key_t key;
    md3_press_t press;
    uint32_t value;   // event argument
} md3_event_t;

/** Queue an event for the UI task; false if the queue is absent or full. */
bool md3_post(const md3_event_t *ev);

/** A coherent status copy. The UI task publishes; USB control readers never touch live backends. */
typedef struct {
    md3_settings_t settings;
    link_state_t link;
    eq_status_t eq;
    bool dac_present;
    int power_vbus_mv;
    bool power_charging;
    bool storage_mounted;
    uint64_t storage_total;
    uint64_t storage_free;
    uint32_t storage_holds;
    char screen[8];
} md3_snapshot_t;

void md3_state_init(void);
void md3_state_publish(const md3_snapshot_t *snapshot);
void md3_state_read(md3_snapshot_t *out);
