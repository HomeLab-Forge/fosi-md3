// Serialized audio power sequencing. Before ui_start() app_main owns this; afterwards only
// the UI/control task may call it.
#pragma once

#include "esp_err.h"

/** Apply UAC/mux state and replay the stock DAC power/init sequence. */
esp_err_t audio_power_cycle(void);
