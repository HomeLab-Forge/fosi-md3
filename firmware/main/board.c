#include "board.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_gc9a01.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"

static const char *TAG = "board";

#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_RES     LEDC_TIMER_10_BIT

void board_gpio_init(void)
{
    const gpio_config_t keys = {
        .pin_bit_mask = BIT64(PIN_KEY_VOL_UP) | BIT64(PIN_KEY_MENU) | BIT64(PIN_KEY_VOL_DOWN) |
                        BIT64(PIN_KEY_VISTA),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&keys));

    const gpio_config_t outputs = {
        .pin_bit_mask = BIT64(PIN_PWR_B) | BIT64(PIN_PWR_A) | BIT64(PIN_AMP_EN) |
                        BIT64(PIN_UAC_SELECT) | BIT64(PIN_DAC_RESET) | BIT64(PIN_USB_SRC) |
                        BIT64(PIN_USB_MUX),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&outputs));

    const gpio_config_t sense = {
        .pin_bit_mask = BIT64(PIN_VBUS_ADC) | BIT64(PIN_DETECT),
        .mode = GPIO_MODE_INPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&sense));

    gpio_reset_pin(PIN_BRIDGE_RST);
    gpio_set_direction(PIN_BRIDGE_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BRIDGE_RST, 0);

    gpio_set_level(PIN_DAC_RESET, 0);
    gpio_set_level(PIN_DAC_RESET, 1);
    gpio_set_level(PIN_USB_SRC, 0);
    gpio_set_level(PIN_USB_MUX, 0);
    gpio_set_level(PIN_PWR_A, 0);
    gpio_set_level(PIN_PWR_B, 0);
    vTaskDelay(1);
}

void board_amp_set(bool on)
{
    gpio_set_level(PIN_AMP_EN, on);
}

bool board_vista_held(void)
{
    return gpio_get_level(PIN_KEY_VISTA) == 0;
}

void board_usb_mux_set_audio(bool audio)
{
    gpio_set_level(PIN_USB_MUX, audio ? 1 : 0);
}

void board_uac_select(bool uac1)
{
    gpio_set_level(PIN_UAC_SELECT, uac1 ? 1 : 0);
}

void board_audio_power_begin(void)
{
    gpio_set_level(PIN_PWR_B, 0);
    gpio_set_level(PIN_AMP_EN, 0);
    gpio_set_direction(PIN_BRIDGE_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BRIDGE_RST, 0);
    gpio_set_level(PIN_PWR_A, 1);
}

void board_audio_power_finish(void)
{
    gpio_set_level(PIN_PWR_B, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_AMP_EN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_direction(PIN_BRIDGE_RST, GPIO_MODE_INPUT);
}

esp_err_t board_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
    };
    return ledc_channel_config(&channel);
}

void board_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, percent * 1023 / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

esp_err_t board_lcd_init(board_lcd_t *out)
{
    const spi_bus_config_t bus = GC9A01_PANEL_BUS_SPI_CONFIG(PIN_LCD_SCLK, PIN_LCD_MOSI,
                                                             LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    const esp_lcd_panel_io_spi_config_t io_cfg = GC9A01_PANEL_IO_SPI_CONFIG(PIN_LCD_CS, PIN_LCD_DC,
                                                                            NULL, NULL);
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg,
                                                 &out->io), TAG, "panel io");

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_gc9a01(out->io, &panel_cfg, &out->panel), TAG, "panel");
    esp_lcd_panel_reset(out->panel);
    esp_lcd_panel_init(out->panel);
    // The MD3's upright orientation needs a vertical reflection; user rotation stays in LVGL.
    esp_lcd_panel_mirror(out->panel, false, true);
    esp_lcd_panel_invert_color(out->panel, true);
    esp_lcd_panel_disp_on_off(out->panel, true);
    ESP_LOGI(TAG, "GC9A01 ready");
    return ESP_OK;
}

void board_reboot_to_download(void)
{
    ESP_LOGW(TAG, "rebooting into ROM download mode");
    board_usb_mux_set_audio(false);
    vTaskDelay(pdMS_TO_TICKS(50));
    // TinyUSB maps the internal USB PHY to USB-OTG through RTC registers that survive
    // esp_restart(). Return it to the eFuse default, USB-Serial-JTAG, where the ROM's
    // download mode enumerates.
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL);
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_restart();
}
