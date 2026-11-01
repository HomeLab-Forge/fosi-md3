// The four physical buttons, posted to the UI queue as EV_KEY events.
#pragma once

#include "esp_err.h"

esp_err_t keys_init(void);
