#include "link.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/i2c_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "link";

typedef struct {
    uint8_t reg;
    uint8_t val;
} link_write_t;

static i2c_slave_dev_handle_t s_slave;
static QueueHandle_t s_queue;
static uint8_t s_regs[256];
static bool s_decoded;
static bool (*s_notify)(void);  // called from the ISR after each queued write; see link_set_notify()

link_state_t g_link = {.rate_label = "--"};

static bool on_receive(i2c_slave_dev_handle_t slave, const i2c_slave_rx_done_event_data_t *evt, void *arg)
{
    if (!s_queue || evt->length < 2) {
        return false;
    }
    const link_write_t w = {.reg = evt->buffer[0], .val = evt->buffer[1]};
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_queue, &w, &woken);
    if (s_notify && s_notify()) {
        woken = pdTRUE;
    }
    return woken == pdTRUE;
}

static bool on_request(i2c_slave_dev_handle_t slave, const i2c_slave_request_event_data_t *evt, void *arg)
{
    return false;  // the bridge never reads; stock firmware ignores requests too
}

esp_err_t link_init(void)
{
    s_decoded = false;
    s_queue = xQueueCreate(64, sizeof(link_write_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    const i2c_slave_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_LINK_SDA,
        .scl_io_num = PIN_LINK_SCL,
        .clk_source = I2C_CLK_SRC_XTAL,
        .send_buf_depth = 100,
        .receive_buf_depth = 100,
        .slave_addr = LINK_I2C_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };
    ESP_RETURN_ON_ERROR(i2c_new_slave_device(&cfg, &s_slave), TAG, "slave");
    const i2c_slave_event_callbacks_t cbs = {.on_receive = on_receive, .on_request = on_request};
    ESP_RETURN_ON_ERROR(i2c_slave_register_event_callbacks(s_slave, &cbs, NULL), TAG, "callbacks");
    ESP_LOGI(TAG, "listening as 0x%02x", LINK_I2C_ADDR);
    return ESP_OK;
}

static uint32_t pcm_rate(uint8_t code)
{
    switch (code & 0x7F) {
    case 0x40:
    case 0x0A: return 44100;
    case 0x00: return 48000;
    case 0x48: return 88200;
    case 0x08: return 96000;
    case 0x58: return 176400;
    case 0x18: return 192000;
    case 0x78: return 352800;
    case 0x38: return 384000;
    default: return 0;
    }
}

static void decode_format(link_state_t *s)
{
    const bool dsd = s_regs[0x10] & 0x02;
    const uint8_t native = s_regs[0x1E];
    const uint8_t code = s_regs[0x11] & 0x7F;
    s->dsd = dsd;
    if (dsd && (native & 0x01)) {
        const uint8_t kind = native & 0x71;
        const int mult = kind == 0x11 ? 64 : kind == 0x21 ? 128 : kind == 0x41 ? 256 : 0;
        s->rate_hz = mult * 44100;
        snprintf(s->rate_label, sizeof(s->rate_label), mult ? "DSD%d" : "DSD", mult);
    } else if (dsd) {
        const int mult = code == 0x58 ? 64 : code == 0x78 ? 128 : 0;  // DoP
        s->rate_hz = mult * 44100;
        snprintf(s->rate_label, sizeof(s->rate_label), mult ? "DSD%d" : "DSD", mult);
    } else {
        s->rate_hz = pcm_rate(code);
        if (s->rate_hz) {
            const uint32_t k = s->rate_hz / 1000;
            const uint32_t frac = (s->rate_hz % 1000) / 100;
            if (frac) {
                snprintf(s->rate_label, sizeof(s->rate_label), "PCM %lu.%luk", (unsigned long)k,
                         (unsigned long)frac);
            } else {
                snprintf(s->rate_label, sizeof(s->rate_label), "PCM %luk", (unsigned long)k);
            }
        } else {
            snprintf(s->rate_label, sizeof(s->rate_label), "--");
        }
    }
}

void link_set_notify(bool (*notify_from_isr)(void))
{
    s_notify = notify_from_isr;
}

bool link_poll(link_state_t *out)
{
    if (!s_queue || !out) {
        return false;
    }
    link_write_t w;
    bool received = false;
    bool changed = false;
    while (xQueueReceive(s_queue, &w, 0) == pdTRUE) {
        received = true;
        ESP_LOGD(TAG, "rec 0x%02x,0x%02x", w.reg, w.val);
        if (s_regs[w.reg] != w.val) {
            changed = true;
        }
        s_regs[w.reg] = w.val;
        out->writes++;
    }
    if (received && (changed || !s_decoded)) {
        out->host_vol_l = s_regs[0x20];
        out->host_vol_r = s_regs[0x21];
        out->host_mute = s_regs[0x22] == 1;
        decode_format(out);
        s_decoded = true;
        return true;
    }
    return changed;
}

void link_regfile(uint8_t out[256])
{
    memcpy(out, s_regs, sizeof(s_regs));
}
