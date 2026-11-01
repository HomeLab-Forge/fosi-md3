#include <stdlib.h>
#include <string.h>

#include "driver/i2c_slave.h"
#include "freertos/queue.h"

typedef struct host_queue {
    size_t item_size;
    size_t capacity;
    size_t count;
    size_t head;
    size_t tail;
    unsigned char data[];
} host_queue_t;

QueueHandle_t xQueueCreate(unsigned int length, unsigned int item_size)
{
    host_queue_t *queue = calloc(1, sizeof(*queue) + (size_t)length * item_size);
    if (!queue) {
        return NULL;
    }
    queue->item_size = item_size;
    queue->capacity = length;
    return queue;
}

BaseType_t xQueueReceive(QueueHandle_t queue, void *out, BaseType_t wait)
{
    (void)wait;
    if (!queue || queue->count == 0) {
        return pdFALSE;
    }
    memcpy(out, queue->data + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1) % queue->capacity;
    queue->count--;
    return pdTRUE;
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void *item, BaseType_t *woken)
{
    (void)woken;
    if (!queue || queue->count == queue->capacity) {
        return pdFALSE;
    }
    memcpy(queue->data + queue->tail * queue->item_size, item, queue->item_size);
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->count++;
    return pdTRUE;
}

esp_err_t i2c_new_slave_device(const i2c_slave_config_t *config, i2c_slave_dev_handle_t *out)
{
    (void)config;
    *out = (i2c_slave_dev_handle_t)1;
    return ESP_OK;
}

esp_err_t i2c_slave_register_event_callbacks(
    i2c_slave_dev_handle_t device, const i2c_slave_event_callbacks_t *callbacks, void *arg)
{
    (void)device;
    (void)callbacks;
    (void)arg;
    return ESP_OK;
}
