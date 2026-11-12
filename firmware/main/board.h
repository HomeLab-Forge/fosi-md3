// Fosi MD3 board definition. Pin roles come from the stock firmware (docs/stock-firmware.md).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

// LCD: GC9A01 on SPI2
#define PIN_LCD_MOSI      GPIO_NUM_11
#define PIN_LCD_SCLK      GPIO_NUM_12
#define PIN_LCD_DC        GPIO_NUM_13
#define PIN_LCD_CS        GPIO_NUM_14
#define PIN_LCD_BL        GPIO_NUM_15
#define PIN_LCD_RST       GPIO_NUM_16
#define LCD_H_RES         240
#define LCD_V_RES         240

// Buttons, active low with pull-ups
#define PIN_KEY_VOL_UP    GPIO_NUM_0
#define PIN_KEY_MENU      GPIO_NUM_8
#define PIN_KEY_VOL_DOWN  GPIO_NUM_9
#define PIN_KEY_VISTA     GPIO_NUM_10

// I2C master to the ES9039Q2M
#define PIN_DAC_SDA       GPIO_NUM_45
#define PIN_DAC_SCL       GPIO_NUM_46
#define PIN_DAC_RESET     GPIO_NUM_38

// I2C slave link from the Savitech USB-audio bridge
#define PIN_LINK_SDA      GPIO_NUM_41
#define PIN_LINK_SCL      GPIO_NUM_42
#define LINK_I2C_ADDR     0x6B

// USB and power control (roles of 3, 4, 7, 17, 47 inferred; sequencing copied verbatim)
#define PIN_USB_MUX       GPIO_NUM_48   // 0 = ESP32 owns the port, 1 = Savitech
#define PIN_UAC_SELECT    GPIO_NUM_18   // 0 = UAC 2.0, 1 = UAC 1.0
#define PIN_USB_SRC       GPIO_NUM_47
#define PIN_PWR_A         GPIO_NUM_4    // high at power-up (probably DAC power)
#define PIN_PWR_B         GPIO_NUM_3    // high after DAC init
#define PIN_AMP_EN        GPIO_NUM_17   // high ~100 ms after DAC init
#define PIN_BRIDGE_RST    GPIO_NUM_7    // held low, then released to input
#define PIN_VBUS_ADC      GPIO_NUM_5    // ADC1 channel 4, vBus = mV * 11
#define PIN_DETECT        GPIO_NUM_6    // unknown detect input

typedef struct {
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
} board_lcd_t;

/** Configure every GPIO exactly as the stock firmware does at boot. */
void board_gpio_init(void);

/** True when the Vista key is held (stock "IAP" check: hold Vista at plug-in to stay flashable). */
bool board_vista_held(void);

/** Headphone amps on or off: GPIO17, taken to be the four ES9603Qs' AMP_PDB (active-low power-down,
 *  pop-free, < 5 uA off). board_audio_power_finish() turns them on. */
void board_amp_set(bool on);

/** Route the USB-C data lines: true = Savitech (audio), false = ESP32. */
void board_usb_mux_set_audio(bool audio);

/** Drive the UAC 1.0/2.0 strap to the Savitech. */
void board_uac_select(bool uac1);

/** First half of the stock audio power-up; call dac_init() between this and board_audio_power_finish(). */
void board_audio_power_begin(void);

/** Second half: raise the analog rail now, the amp 100 ms later, release the bridge 150 ms later. */
void board_audio_power_finish(void);

esp_err_t board_backlight_init(void);
void board_backlight_set(int percent);

esp_err_t board_lcd_init(board_lcd_t *out);

/** Reboot into the ROM download mode with the USB port handed to the ESP32 (for hands-free flashing). */
void board_reboot_to_download(void);
