idf_component_register(
    SRCS
        "main.c"
        "md3_state.c"
        "audio.c"
        "board.c"
        "settings.c"
        "dac.c"
        "eq.c"
        "link.c"
        "storage.c"
        "usb_msc.c"
        "usb_ctl.c"
        "keys.c"
        "logbuf.c"
        "power.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_ledc
        esp_driver_spi
        esp_lcd
        esp_partition
        esp_pm
        esp_timer
        fatfs
        nvs_flash
        wear_levelling
        espressif__button
        espressif__esp_lcd_gc9a01
        espressif__esp_lvgl_port
        espressif__esp_tinyusb
        espressif__tinyusb
        lvgl__lvgl
)

# The stock MSC helper can call f_mkfs on mount failure. It is not allowed in this build;
# main/usb_msc.c supplies the non-formatting implementation instead.
idf_component_get_property(esp_tinyusb_lib espressif__esp_tinyusb COMPONENT_LIB)
get_target_property(esp_tinyusb_sources ${esp_tinyusb_lib} SOURCES)
list(FILTER esp_tinyusb_sources EXCLUDE REGEX "tusb_msc_storage\\.c$")
set_property(TARGET ${esp_tinyusb_lib} PROPERTY SOURCES "${esp_tinyusb_sources}")
