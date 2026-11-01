#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tud_msc_set_sense(uint8_t lun, uint8_t sense_key, uint8_t add_sense_code, uint8_t add_sense_qualifier);
