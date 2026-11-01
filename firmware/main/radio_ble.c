#include "radio_ble.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "md3.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "radio_ble";
static const char *DEVICE_NAME = "MD3 Radio";
static uint8_t s_own_addr_type;
static int s_status;

extern void ble_store_config_init(void);

static void report_status(int status)
{
    s_status = status;
    const md3_event_t ev = {.type = EV_NET};
    (void)md3_post(&ev);
}

int radio_ble_status(void)
{
    return s_status;
}

static void start_advertising(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no BLE identity address: %d", rc);
        report_status(-rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot infer BLE address type: %d", rc);
        report_status(-rc);
        return;
    }

    const char *name = ble_svc_gap_device_name();
    const struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (uint8_t *)name,
        .name_len = strlen(name),
        .name_is_complete = 1,
        .tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO,
        .tx_pwr_lvl_is_present = 1,
    };
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot set advertisement: %d", rc);
        report_status(-rc);
        return;
    }
    const struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot start advertisement: %d", rc);
        report_status(-rc);
        return;
    }
    report_status(1);
    ESP_LOGI(TAG, "advertising as %s", DEVICE_NAME);
}

static void on_stack_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset: %d", reason);
    report_status(-reason);
}

static void on_stack_sync(void)
{
    start_advertising();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

esp_err_t radio_ble_start(void)
{
    esp_err_t result = nimble_port_init();
    if (result != ESP_OK) {
        report_status(-result);
        return result;
    }
    ble_svc_gap_init();
    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc != 0) {
        report_status(-rc);
        return ESP_FAIL;
    }
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    result = xTaskCreate(host_task, "md3_ble", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
    if (result != ESP_OK) {
        report_status(-result);
    }
    return result;
}
