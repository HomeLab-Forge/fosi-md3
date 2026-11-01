#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define I2C_NUM_0 0
#define I2C_CLK_SRC_XTAL 0
#define I2C_ADDR_BIT_LEN_7 0

typedef void *i2c_slave_dev_handle_t;

typedef struct {
    uint8_t *buffer;
    size_t length;
} i2c_slave_rx_done_event_data_t;

typedef struct {
    int unused;
} i2c_slave_request_event_data_t;

typedef struct {
    int i2c_port;
    int sda_io_num;
    int scl_io_num;
    int clk_source;
    int send_buf_depth;
    int receive_buf_depth;
    int slave_addr;
    int addr_bit_len;
} i2c_slave_config_t;

typedef bool (*i2c_slave_received_callback_t)(
    i2c_slave_dev_handle_t, const i2c_slave_rx_done_event_data_t *, void *);
typedef bool (*i2c_slave_request_callback_t)(
    i2c_slave_dev_handle_t, const i2c_slave_request_event_data_t *, void *);

typedef struct {
    i2c_slave_received_callback_t on_receive;
    i2c_slave_request_callback_t on_request;
} i2c_slave_event_callbacks_t;

esp_err_t i2c_new_slave_device(const i2c_slave_config_t *config, i2c_slave_dev_handle_t *out);
esp_err_t i2c_slave_register_event_callbacks(
    i2c_slave_dev_handle_t device, const i2c_slave_event_callbacks_t *callbacks, void *arg);
