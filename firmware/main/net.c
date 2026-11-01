#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "logbuf.h"
#include "lwip/ip4_addr.h"
#include "md3.h"
#include "mdns.h"
#include "nvs.h"
#include "ui.h"

static const char *TAG = "net";

#define API_TOKEN_HEADER "X-MD3-Token"
#define MAX_TEXT_BYTES 2048
#define MAX_JSON_BYTES 4096
#define MAX_IMAGE_BYTES (2 * 1024 * 1024)
#define FRAME_BYTES (LCD_H_RES * LCD_V_RES * 2)
#define LOG_BUFFER_BYTES 8192
#define RECONNECT_MAX_MS 30000

static char s_token[129];
static httpd_handle_t s_http;
static esp_netif_t *s_sta;
static esp_event_handler_instance_t s_wifi_events;
static esp_event_handler_instance_t s_ip_events;
static uint32_t s_reconnect_ms = 1000;
static int64_t s_next_reconnect_us;
static bool s_reconnect_requested;
static bool s_wifi_started;
static bool s_network_initialized;
static int64_t s_last_rssi_us;
static int64_t s_next_http_retry_us;
static uint32_t s_http_retry_ms = 1000;
static bool s_net_dirty;
static portMUX_TYPE s_net_dirty_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t send_json(httpd_req_t *req, const char *status, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    const esp_err_t result = httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
    cJSON_free(text);
    return result;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddStringToObject(root, "error", message);
    return send_json(req, status, root);
}

static esp_err_t send_accepted(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddBoolToObject(root, "accepted", true);
    return send_json(req, "202 Accepted", root);
}

static bool authorized(httpd_req_t *req)
{
    const size_t expected = strlen(s_token);
    const size_t len = httpd_req_get_hdr_value_len(req, API_TOKEN_HEADER);
    if (!expected || len != expected || len >= 129) {
        return false;
    }
    char provided[129];
    if (httpd_req_get_hdr_value_str(req, API_TOKEN_HEADER, provided, len + 1) != ESP_OK) {
        return false;
    }
    uint8_t different = 0;
    for (size_t i = 0; i < expected; i++) {
        different |= (uint8_t)(provided[i] ^ s_token[i]);
    }
    return different == 0;
}

static bool receive_body(httpd_req_t *req, size_t maximum, bool nul_terminate,
                         uint8_t **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (httpd_req_get_hdr_value_len(req, "Transfer-Encoding")) {
        send_error(req, "400 Bad Request", "content-length required");
        return false;
    }
    const size_t len = req->content_len;
    if (len > maximum) {
        send_error(req, "413 Content Too Large", "body too large");
        return false;
    }
    uint8_t *body = heap_caps_malloc(len + (nul_terminate ? 1 : 0), MALLOC_CAP_SPIRAM);
    if (!body) {
        send_error(req, "503 Service Unavailable", "request buffer unavailable");
        return false;
    }
    size_t received = 0;
    while (received < len) {
        const int result = httpd_req_recv(req, (char *)body + received, len - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            free(body);
            send_error(req, "408 Request Timeout", "body timeout");
            return false;
        }
        if (result <= 0) {
            free(body);
            send_error(req, "400 Bad Request", "incomplete body");
            return false;
        }
        received += (size_t)result;
    }
    if (nul_terminate) {
        body[len] = '\0';
    }
    *out = body;
    *out_len = len;
    return true;
}

static bool read_header(httpd_req_t *req, const char *name, char *out, size_t cap)
{
    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (!len || len >= cap) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, name, out, cap) == ESP_OK;
}

static bool object_has_only(cJSON *root, const char *const *allowed, size_t count)
{
    if (!cJSON_IsObject(root)) {
        return false;
    }
    for (cJSON *item = root->child; item; item = item->next) {
        bool found = false;
        for (size_t i = 0; i < count; i++) {
            if (item->string && !strcmp(item->string, allowed[i])) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool post_event(httpd_req_t *req, md3_event_t *ev)
{
    if (!md3_post(ev)) {
        send_error(req, "503 Service Unavailable", "event queue full");
        return false;
    }
    send_accepted(req);
    return true;
}

static esp_err_t handle_status(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    md3_snapshot_t snapshot;
    md3_state_read(&snapshot);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    }
    cJSON_AddStringToObject(root, "firmware", MD3_FW_VERSION);
    cJSON_AddStringToObject(root, "screen", snapshot.screen);
    cJSON_AddBoolToObject(root, "dac_present", snapshot.dac_present);

    cJSON *network = cJSON_AddObjectToObject(root, "network");
    cJSON_AddBoolToObject(network, "connected", snapshot.net.connected);
    cJSON_AddStringToObject(network, "ip", snapshot.net.ip);
    cJSON_AddNumberToObject(network, "rssi", snapshot.net.rssi);

    cJSON *settings = cJSON_AddObjectToObject(root, "settings");
    cJSON_AddNumberToObject(settings, "volume", snapshot.settings.volume);
    cJSON_AddNumberToObject(settings, "filter", snapshot.settings.filter);
    cJSON_AddBoolToObject(settings, "gain_high", snapshot.settings.gain_high);
    cJSON_AddBoolToObject(settings, "vol_from_host", snapshot.settings.vol_from_host);
    cJSON_AddNumberToObject(settings, "brightness", snapshot.settings.brightness);
    cJSON_AddNumberToObject(settings, "rotation", snapshot.settings.rotation);
    cJSON_AddBoolToObject(settings, "uac1", snapshot.settings.uac1);

    cJSON *link = cJSON_AddObjectToObject(root, "link");
    cJSON_AddNumberToObject(link, "host_vol_l", snapshot.link.host_vol_l);
    cJSON_AddNumberToObject(link, "host_vol_r", snapshot.link.host_vol_r);
    cJSON_AddBoolToObject(link, "host_mute", snapshot.link.host_mute);
    cJSON_AddBoolToObject(link, "dsd", snapshot.link.dsd);
    cJSON_AddNumberToObject(link, "rate_hz", snapshot.link.rate_hz);
    cJSON_AddStringToObject(link, "rate_label", snapshot.link.rate_label);
    cJSON_AddNumberToObject(link, "writes", snapshot.link.writes);

    cJSON *power = cJSON_AddObjectToObject(root, "power");
    cJSON_AddNumberToObject(power, "vbus_mv", snapshot.power_vbus_mv);
    cJSON_AddBoolToObject(power, "charging", snapshot.power_charging);

    cJSON *storage = cJSON_AddObjectToObject(root, "storage");
    cJSON_AddBoolToObject(storage, "mounted", snapshot.storage_mounted);
    cJSON_AddNumberToObject(storage, "total_bytes", snapshot.storage_total);
    cJSON_AddNumberToObject(storage, "free_bytes", snapshot.storage_free);
    return send_json(req, "200 OK", root);
}

static esp_err_t handle_logs(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    char *logs = heap_caps_malloc(LOG_BUFFER_BYTES + 1, MALLOC_CAP_SPIRAM);
    if (!logs) {
        return send_error(req, "503 Service Unavailable", "log buffer unavailable");
    }
    const size_t len = logbuf_read(logs, LOG_BUFFER_BYTES + 1);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    const esp_err_t result = httpd_resp_send(req, logs, len);
    free(logs);
    return result;
}

static esp_err_t handle_screenshot(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    uint8_t *bmp = NULL;
    size_t len = 0;
    const esp_err_t result = ui_screenshot_bmp(&bmp, &len);
    if (result != ESP_OK) {
        return send_error(req, "503 Service Unavailable", "screenshot unavailable");
    }
    httpd_resp_set_type(req, "image/bmp");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    const esp_err_t sent = httpd_resp_send(req, (const char *)bmp, len);
    free(bmp);
    return sent;
}

static esp_err_t handle_home(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    if (req->content_len || httpd_req_get_hdr_value_len(req, "Transfer-Encoding")) {
        return send_error(req, "400 Bad Request", "empty body required");
    }
    md3_event_t ev = {.type = EV_HOME};
    post_event(req, &ev);
    return ESP_OK;
}

static bool parse_color(const char *text, uint32_t *out)
{
    if (!text || text[0] != '#' || strlen(text) != 7) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = 1; i < 7; i++) {
        const char c = text[i];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        value = (value << 4) | (uint32_t)digit;
    }
    *out = value;
    return true;
}

static esp_err_t handle_text(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    char content_type[64];
    if (!read_header(req, "Content-Type", content_type, sizeof(content_type)) ||
        strncmp(content_type, "text/plain", 10)) {
        return send_error(req, "415 Unsupported Media Type", "text/plain required");
    }
    uint32_t color = 0xF0F0F0;
    char color_text[16];
    const size_t color_len = httpd_req_get_hdr_value_len(req, "X-MD3-Color");
    if (color_len) {
        if (color_len >= sizeof(color_text) ||
            httpd_req_get_hdr_value_str(req, "X-MD3-Color", color_text, sizeof(color_text)) != ESP_OK ||
            !parse_color(color_text, &color)) {
            return send_error(req, "400 Bad Request", "invalid color");
        }
    }
    uint8_t *body = NULL;
    size_t len = 0;
    if (!receive_body(req, MAX_TEXT_BYTES, true, &body, &len)) {
        return ESP_OK;
    }
    md3_event_t ev = {
        .type = EV_SHOW_TEXT,
        .data = body,
        .len = len + 1,
        .color = color,
    };
    post_event(req, &ev);
    return ESP_OK;
}

static esp_err_t handle_image(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    char content_type[64];
    if (!read_header(req, "Content-Type", content_type, sizeof(content_type))) {
        return send_error(req, "415 Unsupported Media Type", "image content type required");
    }
    md3_event_type_t type;
    if (!strcmp(content_type, "image/jpeg")) {
        type = EV_SHOW_JPEG;
    } else if (!strcmp(content_type, "image/gif")) {
        type = EV_SHOW_GIF;
    } else if (!strcmp(content_type, "application/x-rgb565")) {
        type = EV_SHOW_FRAME;
    } else {
        return send_error(req, "415 Unsupported Media Type", "unsupported image type");
    }
    if (!req->content_len) {
        return send_error(req, "400 Bad Request", "nonempty image body required");
    }
    if (type == EV_SHOW_FRAME && req->content_len != FRAME_BYTES) {
        return send_error(req, "422 Unprocessable Content", "raw frame must be 115200 bytes");
    }
    uint8_t *body = NULL;
    size_t len = 0;
    if (!receive_body(req, MAX_IMAGE_BYTES, false, &body, &len)) {
        return ESP_OK;
    }
    if (type == EV_SHOW_GIF && (len < 6 || memcmp(body, "GIF8", 4))) {
        free(body);
        return send_error(req, "422 Unprocessable Content", "invalid GIF signature");
    }
    if (type == EV_SHOW_JPEG && (len < 2 || body[0] != 0xFF || body[1] != 0xD8)) {
        free(body);
        return send_error(req, "422 Unprocessable Content", "invalid JPEG signature");
    }
    md3_event_t ev = {.type = type, .data = body, .len = len};
    post_event(req, &ev);
    return ESP_OK;
}

static bool parse_key_name(const char *name, md3_key_t *out)
{
    static const char *const names[] = {"vol_up", "vol_down", "menu", "vista"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i])) {
            *out = (md3_key_t)i;
            return true;
        }
    }
    return false;
}

static bool parse_press_name(const char *name, md3_press_t *out)
{
    static const char *const names[] = {"click", "double", "long", "hold"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(name, names[i])) {
            *out = (md3_press_t)i;
            return true;
        }
    }
    return false;
}

static esp_err_t handle_key(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!req->content_len) {
        return send_error(req, "400 Bad Request", "JSON body required");
    }
    uint8_t *body = NULL;
    size_t len = 0;
    if (!receive_body(req, MAX_JSON_BYTES, true, &body, &len)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse((const char *)body);
    free(body);
    static const char *const allowed[] = {"key", "press"};
    cJSON *key = cJSON_GetObjectItemCaseSensitive(root, "key");
    cJSON *press = cJSON_GetObjectItemCaseSensitive(root, "press");
    md3_event_t ev = {.type = EV_KEY};
    const bool valid = object_has_only(root, allowed, 2) && cJSON_GetArraySize(root) == 2 &&
                       cJSON_IsString(key) && cJSON_IsString(press) &&
                       parse_key_name(key->valuestring, &ev.key) &&
                       parse_press_name(press->valuestring, &ev.press);
    cJSON_Delete(root);
    if (!valid) {
        return send_error(req, "400 Bad Request", "invalid key event");
    }
    post_event(req, &ev);
    return ESP_OK;
}

static bool set_number(cJSON *root, const char *name, uint32_t mask, int maximum,
                       md3_settings_patch_t *patch, uint8_t *target)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return true;
    }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > maximum || item->valuedouble != (double)(int)item->valuedouble) {
        return false;
    }
    *target = (uint8_t)item->valuedouble;
    patch->mask |= mask;
    return true;
}

static bool set_bool(cJSON *root, const char *name, uint32_t mask,
                     md3_settings_patch_t *patch, uint8_t *target)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *target = cJSON_IsTrue(item) ? 1 : 0;
    patch->mask |= mask;
    return true;
}

static esp_err_t handle_settings(httpd_req_t *req)
{
    if (!authorized(req)) {
        return send_error(req, "401 Unauthorized", "unauthorized");
    }
    if (!req->content_len) {
        return send_error(req, "400 Bad Request", "JSON body required");
    }
    uint8_t *body = NULL;
    size_t len = 0;
    if (!receive_body(req, MAX_JSON_BYTES, true, &body, &len)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_Parse((const char *)body);
    free(body);
    static const char *const allowed[] = {
        "volume", "filter", "gain_high", "vol_from_host", "brightness", "rotation", "uac1",
    };
    md3_settings_patch_t *patch = calloc(1, sizeof(*patch));
    bool valid = patch && object_has_only(root, allowed, sizeof(allowed) / sizeof(allowed[0]));
    if (valid) {
        valid = set_number(root, "volume", MD3_SET_VOLUME, 100, patch, &patch->value.volume) &&
                set_number(root, "filter", MD3_SET_FILTER, 7, patch, &patch->value.filter) &&
                set_bool(root, "gain_high", MD3_SET_GAIN_HIGH, patch, &patch->value.gain_high) &&
                set_bool(root, "vol_from_host", MD3_SET_VOL_FROM_HOST, patch, &patch->value.vol_from_host) &&
                set_number(root, "brightness", MD3_SET_BRIGHTNESS, 4, patch, &patch->value.brightness) &&
                set_number(root, "rotation", MD3_SET_ROTATION, 3, patch, &patch->value.rotation) &&
                set_bool(root, "uac1", MD3_SET_UAC1, patch, &patch->value.uac1) && patch->mask;
    }
    cJSON_Delete(root);
    if (!valid) {
        free(patch);
        return send_error(req, "400 Bad Request", "invalid settings patch");
    }
    md3_event_t ev = {.type = EV_SETTINGS, .data = (uint8_t *)patch, .len = sizeof(*patch)};
    post_event(req, &ev);
    return ESP_OK;
}

static void update_net_state(bool connected, const esp_ip4_addr_t *ip)
{
    md3_net_t state;
    md3_net_get(&state);
    state.connected = connected;
    state.ip[0] = '\0';
    state.rssi = 0;
    if (connected && ip) {
        snprintf(state.ip, sizeof(state.ip), IPSTR, IP2STR(ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            state.rssi = ap.rssi;
        }
    }
    md3_net_set(&state);
    portENTER_CRITICAL(&s_net_dirty_lock);
    s_net_dirty = true;
    portEXIT_CRITICAL(&s_net_dirty_lock);
}

static void publish_scan_result(esp_err_t error, uint16_t count, int best_rssi)
{
    md3_net_t state;
    md3_net_get(&state);
    state.scan_done = true;
    state.scan_error = error;
    state.scan_count = count;
    state.scan_rssi = best_rssi;
    md3_net_set(&state);
    portENTER_CRITICAL(&s_net_dirty_lock);
    s_net_dirty = true;
    portEXIT_CRITICAL(&s_net_dirty_lock);
}

bool net_take_dirty(void)
{
    portENTER_CRITICAL(&s_net_dirty_lock);
    const bool dirty = s_net_dirty;
    s_net_dirty = false;
    portEXIT_CRITICAL(&s_net_dirty_lock);
    return dirty;
}

static esp_err_t start_http_server(void)
{
    if (s_http) {
        return ESP_OK;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_open_sockets = 4;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    esp_err_t result = httpd_start(&s_http, &config);
    if (result != ESP_OK) {
        return result;
    }
    static const httpd_uri_t routes[] = {
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = handle_status},
        {.uri = "/api/v1/logs", .method = HTTP_GET, .handler = handle_logs},
        {.uri = "/api/v1/screenshot.bmp", .method = HTTP_GET, .handler = handle_screenshot},
        {.uri = "/api/v1/display/home", .method = HTTP_POST, .handler = handle_home},
        {.uri = "/api/v1/display/text", .method = HTTP_POST, .handler = handle_text},
        {.uri = "/api/v1/display/image", .method = HTTP_POST, .handler = handle_image},
        {.uri = "/api/v1/key", .method = HTTP_POST, .handler = handle_key},
        {.uri = "/api/v1/settings", .method = HTTP_POST, .handler = handle_settings},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        result = httpd_register_uri_handler(s_http, &routes[i]);
        if (result != ESP_OK) {
            httpd_stop(s_http);
            s_http = NULL;
            return result;
        }
    }
    ESP_LOGI(TAG, "authenticated HTTP API ready on port 80");
    return ESP_OK;
}

static void schedule_http_retry(void)
{
    s_next_http_retry_us = esp_timer_get_time() + (int64_t)s_http_retry_ms * 1000;
    s_http_retry_ms = s_http_retry_ms < RECONNECT_MAX_MS / 2 ? s_http_retry_ms * 2 : RECONNECT_MAX_MS;
}

static void request_reconnect(void)
{
    portENTER_CRITICAL(&s_net_dirty_lock);
    s_reconnect_requested = true;
    portEXIT_CRITICAL(&s_net_dirty_lock);
}

static bool take_reconnect_request(void)
{
    portENTER_CRITICAL(&s_net_dirty_lock);
    const bool requested = s_reconnect_requested;
    s_reconnect_requested = false;
    portEXIT_CRITICAL(&s_net_dirty_lock);
    return requested;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_SCAN_DONE) {
        uint16_t total = 0;
        esp_err_t result = esp_wifi_scan_get_ap_num(&total);
        int best_rssi = -127;
        if (result == ESP_OK && total > 0) {
            uint16_t count = total > 32 ? 32 : total;
            wifi_ap_record_t *records = calloc(count, sizeof(*records));
            if (!records) {
                result = ESP_ERR_NO_MEM;
            } else {
                result = esp_wifi_scan_get_ap_records(&count, records);
                if (result == ESP_OK) {
                    for (uint16_t i = 0; i < count; i++) {
                        if (records[i].rssi > best_rssi) {
                            best_rssi = records[i].rssi;
                        }
                    }
                }
                free(records);
            }
        }
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(result));
        } else {
            ESP_LOGI(TAG, "Wi-Fi scan: %u networks, best RSSI %d dBm", total, best_rssi);
        }
        publish_scan_result(result, total, best_rssi);
        return;
    }
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        update_net_state(false, NULL);
        request_reconnect();
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got = data;
        update_net_state(true, &got->ip_info.ip);
        // Retry timestamps and backoff are owned only by the UI task's net_tick().
    } else if (id == IP_EVENT_STA_LOST_IP) {
        update_net_state(false, NULL);
        request_reconnect();
    }
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *out, size_t cap)
{
    size_t len = cap;
    const esp_err_t result = nvs_get_str(handle, key, out, &len);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "NVS key unavailable: %s (%s)", key, esp_err_to_name(result));
        return result;
    }
    if (!len || len > cap || out[len - 1] != '\0') {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool raw_pmk_valid(const char *password)
{
    for (size_t i = 0; i < 64; i++) {
        const char c = password[i];
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

static esp_err_t load_credentials(wifi_config_t *config)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    nvs_handle_t handle;
    esp_err_t result = nvs_open("md3", NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = read_nvs_string(handle, "ssid", ssid, sizeof(ssid));
        if (result == ESP_OK) result = read_nvs_string(handle, "pass", pass, sizeof(pass));
        if (result == ESP_OK) result = read_nvs_string(handle, "token", s_token, sizeof(s_token));
        nvs_close(handle);
    }

    size_t ssid_len = 0;
    size_t pass_len = 0;
    size_t token_len = 0;
    if (result == ESP_OK) {
        ssid_len = strlen(ssid);
        pass_len = strlen(pass);
        token_len = strlen(s_token);
        if (!ssid_len || pass_len < 8 || token_len < 32) {
            result = ESP_ERR_INVALID_SIZE;
        } else if (pass_len == 64 && !raw_pmk_valid(pass)) {
            result = ESP_ERR_INVALID_ARG;
        }
    }
    if (result == ESP_OK) {
        for (size_t i = 0; i < token_len; i++) {
            if (s_token[i] < 0x21 || s_token[i] > 0x7E) {
                result = ESP_ERR_INVALID_ARG;
                break;
            }
        }
    }
    if (result == ESP_OK) {
        memset(config, 0, sizeof(*config));
        memcpy(config->sta.ssid, ssid, ssid_len);
        memcpy(config->sta.password, pass, pass_len);
        config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        config->sta.pmf_cfg.capable = true;
        config->sta.pmf_cfg.required = false;
    } else {
        memset(config, 0, sizeof(*config));
        memset(s_token, 0, sizeof(s_token));
    }
    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
    return result;
}

static esp_err_t start_mdns(bool *owned)
{
    *owned = false;
    esp_err_t result = mdns_init();
    if (result == ESP_OK) {
        *owned = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        return result;
    }
    ESP_RETURN_ON_ERROR(mdns_hostname_set("md3"), TAG, "mDNS hostname");
    ESP_RETURN_ON_ERROR(mdns_instance_name_set("Fosi MD3"), TAG, "mDNS instance");
    ESP_RETURN_ON_ERROR(mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0), TAG, "mDNS service");
    return ESP_OK;
}

static void unwind_net_start(bool event_loop_owned, bool wifi_initialized, bool mdns_owned)
{
    if (wifi_initialized) {
        (void)esp_wifi_stop();
    }
    if (mdns_owned) {
        mdns_free();
    }
    if (s_wifi_events) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_events);
        s_wifi_events = NULL;
    }
    if (s_ip_events) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_events);
        s_ip_events = NULL;
    }
    if (s_sta) {
        esp_netif_destroy_default_wifi(s_sta);
        s_sta = NULL;
    }
    if (wifi_initialized) {
        (void)esp_wifi_deinit();
    }
    if (event_loop_owned) {
        (void)esp_event_loop_delete_default();
    }
    // esp_netif_deinit() is not supported by ESP-IDF; a later init returns INVALID_STATE,
    // which net_start already accepts. Everything created by this call has been unwound.
    s_wifi_started = false;
    s_network_initialized = false;
    s_http = NULL;
    s_next_http_retry_us = 0;
    s_http_retry_ms = 1000;
    s_reconnect_ms = 1000;
    s_next_reconnect_us = 0;
    s_last_rssi_us = 0;
    portENTER_CRITICAL(&s_net_dirty_lock);
    s_net_dirty = false;
    s_reconnect_requested = false;
    portEXIT_CRITICAL(&s_net_dirty_lock);
}

esp_err_t net_start(void)
{
    if (s_network_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    bool event_loop_owned = false;
    bool wifi_initialized = false;
    bool mdns_owned = false;
    wifi_config_t config;
    esp_err_t result = load_credentials(&config);
    const bool have_credentials = result == ESP_OK;
    if (!have_credentials) {
        ESP_LOGW(TAG, "no provisioned station; running the unauthenticated boot scan only");
    }
    result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) goto fail;
    result = esp_event_loop_create_default();
    if (result == ESP_OK) {
        event_loop_owned = true;
    } else if (result != ESP_ERR_INVALID_STATE) {
        goto fail;
    }
    s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta) {
        result = ESP_FAIL;
        goto fail;
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init);
    if (result != ESP_OK) goto fail;
    wifi_initialized = true;
    result = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &s_wifi_events);
    if (result != ESP_OK) goto fail;
    result = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL, &s_ip_events);
    if (result != ESP_OK) goto fail;
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) goto fail;
    if (have_credentials) {
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        if (result != ESP_OK) goto fail;
    }
    memset(&config, 0, sizeof(config));
    if (have_credentials) {
        result = start_mdns(&mdns_owned);
        if (result != ESP_OK) {
            if (mdns_owned) {
                mdns_free();
                mdns_owned = false;
            }
            // Discovery is optional; direct-IP control must still recover independently.
            ESP_LOGW(TAG, "mDNS unavailable; continuing with IP-only control: %s", esp_err_to_name(result));
        }
    }
    result = esp_wifi_start();
    if (result != ESP_OK) goto fail;
    s_wifi_started = true;
    if (have_credentials) {
        s_next_reconnect_us = esp_timer_get_time() + 1000000;
        s_network_initialized = true;
        result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "initial connect failed; retry scheduled: %s", esp_err_to_name(result));
        }
        ESP_LOGI(TAG, "station started; waiting for IPv4");
        return ESP_OK;
    }
    result = esp_wifi_scan_start(NULL, false);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "could not start Wi-Fi scan: %s", esp_err_to_name(result));
        publish_scan_result(result, 0, -127);
        return result;
    }
    ESP_LOGI(TAG, "Wi-Fi scan started without credentials");
    return ESP_OK;

fail:
    unwind_net_start(event_loop_owned, wifi_initialized, mdns_owned);
    memset(&config, 0, sizeof(config));
    memset(s_token, 0, sizeof(s_token));
    return result;
}

void net_tick(void)
{
    if (!s_network_initialized) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    md3_net_t state;
    md3_net_get(&state);
    if (state.connected) {
        (void)take_reconnect_request();
        s_reconnect_ms = 1000;
        s_next_reconnect_us = 0;
    } else {
        if (take_reconnect_request()) {
            s_next_reconnect_us = 0;
        }
        if (now >= s_next_reconnect_us) {
            const esp_err_t result = esp_wifi_connect();
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "reconnect failed; retry scheduled: %s", esp_err_to_name(result));
            }
            s_next_reconnect_us = now + (int64_t)s_reconnect_ms * 1000;
            s_reconnect_ms = s_reconnect_ms < RECONNECT_MAX_MS / 2 ? s_reconnect_ms * 2 : RECONNECT_MAX_MS;
        }
    }
    if (state.connected && !s_http && now >= s_next_http_retry_us) {
        const esp_err_t result = start_http_server();
        if (result == ESP_OK) {
            s_http_retry_ms = 1000;
            s_next_http_retry_us = 0;
        } else {
            ESP_LOGW(TAG, "HTTP server start failed; retry scheduled: %s", esp_err_to_name(result));
            schedule_http_retry();
        }
    }
    if (now - s_last_rssi_us < 5000000) {
        return;
    }
    s_last_rssi_us = now;
    if (!state.connected) {
        return;
    }
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && state.rssi != ap.rssi) {
        state.rssi = ap.rssi;
        md3_net_set(&state);
    }
}
