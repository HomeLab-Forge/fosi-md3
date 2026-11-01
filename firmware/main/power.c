#include "power.h"

#include "board.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "power";

#define VBUS_CHANNEL      ADC_CHANNEL_4   // GPIO5
#define VBUS_SCALE        11
#define VBUS_CHARGE_MV    3850
#define VBUS_OVER_MV      5500

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static int s_vbus_mv;
static bool s_charging;
static bool s_over_logged;

esp_err_t power_init(void)
{
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit, &s_adc), TAG, "adc unit");
    const adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12};
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, VBUS_CHANNEL, &chan), TAG, "adc channel");
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1,
        .chan = VBUS_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "no ADC calibration; VBUS readings are approximate");
        s_cali = NULL;
    }
    // Establish the initial charge state so the first housekeeping pass does not rerun audio.
    (void)power_tick();
    return ESP_OK;
}

bool power_tick(void)
{
    if (!s_adc) {
        return false;
    }
    int raw = 0, mv = 0;
    if (adc_oneshot_read(s_adc, VBUS_CHANNEL, &raw) != ESP_OK) {
        return false;
    }
    if (s_cali) {
        adc_cali_raw_to_voltage(s_cali, raw, &mv);
    } else {
        mv = raw * 3100 / 4095;
    }
    s_vbus_mv = mv * VBUS_SCALE;
    if (s_vbus_mv > VBUS_OVER_MV && !s_over_logged) {
        ESP_LOGW(TAG, "VBUS high: %d mV", s_vbus_mv);
        s_over_logged = true;
    }

    const bool detect = gpio_get_level(PIN_DETECT);
    const bool charging = s_vbus_mv > VBUS_CHARGE_MV && detect;
    bool rerun = false;
    if (charging != s_charging) {
        ESP_LOGI(TAG, "%s (vbus %d mV)", charging ? "charging" : "only usb pd in", s_vbus_mv);
        s_charging = charging;
        rerun = true;
    }
    gpio_set_level(PIN_USB_SRC, charging ? 0 : detect);
    return rerun;
}

int power_vbus_mv(void)
{
    return s_vbus_mv;
}

bool power_charging(void)
{
    return s_charging;
}
