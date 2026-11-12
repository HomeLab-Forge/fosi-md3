#include "usb_ctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eq.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "logbuf.h"
#include "md3.h"
#include "tinyusb.h"
#include "ui.h"

static const char *TAG = "usb_ctl";

#define REPLY_BYTES (16 * 1024)

static uint8_t *s_reply;   // INFO, LOG and SHOT replies; PSRAM (TinyUSB copies through its EP0 buffer)
static uint8_t *s_shot;    // last captured screenshot
static size_t s_shot_len;

esp_err_t usb_ctl_init(void)
{
    if (!s_reply) {
        s_reply = heap_caps_malloc(REPLY_BYTES, MALLOC_CAP_SPIRAM);
    }
    return s_reply ? ESP_OK : ESP_ERR_NO_MEM;
}

static const char *const k_eq_state[4] = {"builtin", "custom", "fallback", "dsd"};

static size_t info_json(char *out, size_t cap)
{
    md3_snapshot_t s;
    md3_state_read(&s);
    // Usage is only measured while the firmware has the volume mounted; unknown is null, not zero.
    char free_text[24] = "null", total_text[24] = "null";
    if (s.storage_total) {
        snprintf(free_text, sizeof(free_text), "%llu", s.storage_free);
        snprintf(total_text, sizeof(total_text), "%llu", s.storage_total);
    }
    const int n = snprintf(out, cap,
        "{\"firmware\":\"%s\",\"uptime_s\":%lld,\"screen\":\"%s\","
        "\"storage\":{\"owner\":\"%s\",\"holds\":%lu,\"free\":%s,\"total\":%s},"
        "\"udisk_mode\":%s,\"dac_present\":%s,\"amp_on\":%s,"
        "\"power\":{\"vbus_mv\":%d,\"charging\":%s},"
        "\"settings\":{\"volume\":%d,\"gain_high\":%s,\"brightness\":%d},"
        "\"eq\":{\"preset\":\"%s\",\"state\":\"%s\",\"group\":%d,"
        "\"scale\":{\"log2_2x\":%d,\"log2_4x\":%d,\"trim_tenth_db\":%d}},"
        "\"link\":{\"rate\":\"%s\"}}",
        MD3_FW_VERSION, esp_timer_get_time() / 1000000, s.screen,
        s.storage_mounted ? "device" : "host", (unsigned long)s.storage_holds, free_text, total_text,
        s.settings.udisk_mode ? "true" : "false", s.dac_present ? "true" : "false", s.amp_on ? "true" : "false",
        s.power_vbus_mv, s.power_charging ? "true" : "false",
        s.settings.volume, s.settings.gain_high ? "true" : "false", s.settings.brightness,
        eq_preset_name(s.eq.preset), k_eq_state[s.eq.state < 4 ? s.eq.state : 0], s.eq.group,
        s.eq.log2_2x, s.eq.log2_4x, s.eq.trim_tenth_db,
        s.link.rate_label);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static bool post(md3_event_type_t type, md3_key_t key, md3_press_t press, uint32_t value)
{
    const md3_event_t ev = {.type = type, .key = key, .press = press, .value = value};
    return md3_post(&ev);
}

static bool reply(uint8_t rhport, tusb_control_request_t const *request, const void *data, size_t len)
{
    if (len > request->wLength) {
        len = request->wLength;
    }
    return tud_control_xfer(rhport, request, (void *)data, (uint16_t)len);
}

static bool capture_shot(void)
{
    free(s_shot);
    s_shot = NULL;
    s_shot_len = 0;
    const esp_err_t result = ui_screenshot_bmp(&s_shot, &s_shot_len);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "screenshot failed: %s", esp_err_to_name(result));
        return false;
    }
    return true;
}

static bool handle_in(uint8_t rhport, tusb_control_request_t const *request)
{
    switch (request->bRequest) {
    case MD3_CTL_INFO:
        return reply(rhport, request, s_reply, info_json((char *)s_reply, REPLY_BYTES));
    case MD3_CTL_LOG: {
        const size_t cap = request->wLength + 1u < REPLY_BYTES ? request->wLength + 1u : REPLY_BYTES;
        return reply(rhport, request, s_reply, logbuf_read((char *)s_reply, cap));
    }
    case MD3_CTL_SHOT: {
        const uint32_t size = capture_shot() ? (uint32_t)s_shot_len : 0;
        memcpy(s_reply, &size, sizeof(size));
        return reply(rhport, request, s_reply, sizeof(size));
    }
    case MD3_CTL_READ: {
        const size_t offset = ((size_t)request->wIndex << 16) | request->wValue;
        if (!s_shot || offset > s_shot_len) {
            return false;
        }
        return reply(rhport, request, s_shot + offset, s_shot_len - offset);
    }
    default:
        return false;
    }
}

static bool handle_out(tusb_control_request_t const *request)
{
    const uint16_t v = request->wValue, i = request->wIndex;
    switch (request->bRequest) {
    case MD3_CTL_KEY:
        return v <= KEY_VISTA && i <= PRESS_HOLD && post(EV_KEY, (md3_key_t)v, (md3_press_t)i, 0);
    case MD3_CTL_OFFER:
        return post(EV_OFFER, 0, 0, 0);
    case MD3_CTL_EQ_SCALE:
        return eq_scale_valid(v & 0xFF, v >> 8, (int16_t)i) &&
               post(EV_EQ_SCALE, 0, 0, v | (uint32_t)i << 16);
    case MD3_CTL_ROM:
        return v == MD3_CTL_ROM_VALUE && i == MD3_CTL_ROM_INDEX && post(EV_ROM, 0, 0, 0);
    default:
        return false;
    }
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.recipient != TUSB_REQ_RCPT_DEVICE || !s_reply) {
        return false;
    }
    if (request->bmRequestType_bit.direction == TUSB_DIR_IN) {
        return handle_in(rhport, request);
    }
    // OUT requests take no data stage; a request the UI queue cannot take stalls so the host retries.
    return request->wLength == 0 && handle_out(request) && tud_control_status(rhport, request);
}
