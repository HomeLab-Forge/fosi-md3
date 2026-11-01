#pragma once

#define PIN_LINK_SDA 41
#define PIN_LINK_SCL 42
#define LINK_I2C_ADDR 0x6B

void board_audio_power_begin(void);
void board_uac_select(bool uac1);
void board_usb_mux_set_audio(bool audio);
void board_audio_power_finish(void);
