#include "dac.h"

#include "board.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "link.h"
#include "settings.h"

static const char *TAG = "dac";

#define DAC_ADDR        0x48
#define REG_CHIP_ID     0xE1
#define CHIP_ID_9039    0x63
#define REG_FILTER      0x58
#define FILTER_SHAPE_MASK       0x07
#define FILTER_RESERVED_DEFAULT 0xB8  // reset value: reserved [7:3] = 5'b10111, FILTER_SHAPE = 0
#define REG_ATT_L       0x4A
#define REG_ATT_R       0x4B
#define ATT_MUTE        0xFF
#define I2C_TIMEOUT_MS  500

// Init pairs copied from the stock firmware (DRAM 0x3fc99b18), written in this order.
static const uint8_t k_init[][2] = {
    {0x01, 0xB7}, {0x05, 0x00}, {0x1D, 0x40}, {0x40, 0x00}, {0x41, 0x21},
    {0x39, 0x41}, {0x7B, 0x00}, {0x7D, 0x00}, {0x7C, 0x00}, {0x00, 0x02},
};

// Stock 101-step volume curve (DROM 0x3c093d00): attenuation in 0.5 dB units, 0xFF = mute.
static const uint8_t k_vol_table[101] = {
    0xFF, 0xC0, 0xB8, 0xB4, 0xB0, 0xAC, 0xA8, 0xA4, 0xA0, 0x9C, 0x98, 0x94,
    0x90, 0x8C, 0x88, 0x84, 0x80, 0x7C, 0x78, 0x74, 0x70, 0x6C, 0x68, 0x64,
    0x60, 0x5C, 0x58, 0x56, 0x54, 0x52, 0x50, 0x4E, 0x4C, 0x4A, 0x48, 0x46,
    0x44, 0x42, 0x40, 0x3E, 0x3C, 0x3B, 0x3A, 0x39, 0x38, 0x37, 0x36, 0x35,
    0x34, 0x33, 0x32, 0x31, 0x30, 0x2F, 0x2E, 0x2D, 0x2C, 0x2B, 0x2A, 0x29,
    0x28, 0x27, 0x26, 0x25, 0x24, 0x23, 0x22, 0x21, 0x20, 0x1F, 0x1E, 0x1D,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x04, 0x03, 0x02, 0x01, 0x00,
};

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int s_cache_l = -1;
static int s_cache_r = -1;

extern link_state_t g_link;

static esp_err_t write_reg(uint8_t reg, uint8_t val)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val)
{
    if (!s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, I2C_TIMEOUT_MS);
}

// Host volume byte is signed dB: 0 -> 0, -1..-70 dB -> 2 steps per dB, anything else mutes.
static int host_attenuation(uint8_t v)
{
    if (v == 0) {
        return 0;
    }
    if (v >= 0xBA) {
        return (256 - v) * 2;
    }
    return ATT_MUTE;
}

static int channel_attenuation(uint8_t host_vol)
{
    int att = k_vol_table[g_settings.volume > 100 ? 100 : g_settings.volume];
    if (g_settings.vol_from_host) {
        att += g_link.host_mute ? ATT_MUTE : host_attenuation(host_vol);
    }
    if (att > ATT_MUTE) {
        att = ATT_MUTE;
    }
    if (!g_settings.gain_high) {
        att = (att > 0xF2) ? ATT_MUTE : att + 12;
    }
    return att;
}

esp_err_t dac_init(void)
{
    if (!s_bus) {
        const i2c_master_bus_config_t bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = PIN_DAC_SDA,
            .scl_io_num = PIN_DAC_SCL,
            .clk_source = I2C_CLK_SRC_XTAL,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "bus");
    }
    if (!s_dev) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = DAC_ADDR,
            .scl_speed_hz = 100000,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "device");
    }

    gpio_set_level(PIN_DAC_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_DAC_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_present = false;
    for (int i = 0; i < 10 && !s_present; i++) {
        uint8_t id = 0;
        if (read_reg(REG_CHIP_ID, &id) == ESP_OK && id == CHIP_ID_9039) {
            s_present = true;
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!s_present) {
        ESP_LOGE(TAG, "ES9039 not found");
        return ESP_ERR_NOT_FOUND;
    }
    for (size_t i = 0; i < sizeof(k_init) / sizeof(k_init[0]); i++) {
        ESP_RETURN_ON_ERROR(write_reg(k_init[i][0], k_init[i][1]), TAG, "init reg %02x", k_init[i][0]);
        uint8_t back = 0;
        read_reg(k_init[i][0], &back);
        ESP_LOGI(TAG, "reg 0x%02x = 0x%02x", k_init[i][0], back);
    }
    dac_apply_filter();
    dac_invalidate();
    dac_apply_volume();
    ESP_LOGI(TAG, "ES9039Q2M ready");
    return ESP_OK;
}

void dac_apply_volume(void)
{
    if (!s_present) {
        return;
    }
    const int l = channel_attenuation(g_link.host_vol_l);
    const int r = channel_attenuation(g_link.host_vol_r);
    if (l != s_cache_l && write_reg(REG_ATT_L, (uint8_t)l) == ESP_OK) {
        s_cache_l = l;
    }
    if (r != s_cache_r && write_reg(REG_ATT_R, (uint8_t)r) == ESP_OK) {
        s_cache_r = r;
    }
}

void dac_apply_filter(void)
{
    if (!s_present) {
        return;
    }
    // FILTER_SHAPE is bits [2:0]; bits [7:3] are reserved with a reset value of 5'b10111, so keep
    // whatever the chip holds there (or that default if the read fails).
    uint8_t reg = FILTER_RESERVED_DEFAULT;
    if (read_reg(REG_FILTER, &reg) != ESP_OK) {
        reg = FILTER_RESERVED_DEFAULT;
    }
    write_reg(REG_FILTER, (reg & ~FILTER_SHAPE_MASK) | (g_settings.filter & FILTER_SHAPE_MASK));
}

void dac_invalidate(void)
{
    s_cache_l = -1;
    s_cache_r = -1;
}

bool dac_present(void)
{
    return s_present;
}
