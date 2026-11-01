#include "keys.h"

#include "board.h"
#include "button_gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "iot_button.h"
#include "md3.h"

static const char *TAG = "keys";

typedef struct {
    md3_key_t key;
    md3_press_t press;
} key_binding_t;

// One static binding per (key, press) so the callback's user data never dangles.
static key_binding_t s_bindings[4][4];

static void on_button(void *handle, void *usr)
{
    const key_binding_t *b = usr;
    const md3_event_t ev = {.type = EV_KEY, .key = b->key, .press = b->press};
    md3_post(&ev);
}

static esp_err_t add_key(md3_key_t key, gpio_num_t pin, uint16_t short_ms, bool repeat, bool dbl)
{
    const button_config_t cfg = {.long_press_time = 800, .short_press_time = short_ms};
    const button_gpio_config_t gpio_cfg = {.gpio_num = pin, .active_level = 0};
    button_handle_t btn;
    ESP_RETURN_ON_ERROR(iot_button_new_gpio_device(&cfg, &gpio_cfg, &btn), TAG, "key %d", key);

    s_bindings[key][PRESS_CLICK] = (key_binding_t){key, PRESS_CLICK};
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, on_button, &s_bindings[key][PRESS_CLICK]);
    s_bindings[key][PRESS_LONG] = (key_binding_t){key, PRESS_LONG};
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, on_button, &s_bindings[key][PRESS_LONG]);
    if (repeat) {
        s_bindings[key][PRESS_HOLD] = (key_binding_t){key, PRESS_HOLD};
        iot_button_register_cb(btn, BUTTON_LONG_PRESS_HOLD, NULL, on_button, &s_bindings[key][PRESS_HOLD]);
    }
    if (dbl) {
        s_bindings[key][PRESS_DOUBLE] = (key_binding_t){key, PRESS_DOUBLE};
        iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, on_button, &s_bindings[key][PRESS_DOUBLE]);
    }
    return ESP_OK;
}

esp_err_t keys_init(void)
{
    ESP_RETURN_ON_ERROR(add_key(KEY_VOL_UP, PIN_KEY_VOL_UP, 180, true, false), TAG, "vol+");
    ESP_RETURN_ON_ERROR(add_key(KEY_VOL_DOWN, PIN_KEY_VOL_DOWN, 180, true, false), TAG, "vol-");
    ESP_RETURN_ON_ERROR(add_key(KEY_MENU, PIN_KEY_MENU, 180, false, false), TAG, "menu");
    ESP_RETURN_ON_ERROR(add_key(KEY_VISTA, PIN_KEY_VISTA, 300, false, true), TAG, "vista");
    ESP_LOGI(TAG, "4 keys ready");
    return ESP_OK;
}
