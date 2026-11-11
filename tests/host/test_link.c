#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.c"

static void write_pair(uint8_t reg, uint8_t val)
{
    uint8_t bytes[2] = {reg, val};
    i2c_slave_rx_done_event_data_t event = {.buffer = bytes, .length = sizeof(bytes)};
    assert(on_receive(NULL, &event, NULL) == false);
}

static int notify_calls;

static bool notify_stub(void)
{
    notify_calls++;
    return true;
}

int main(void)
{
    assert(link_init() == ESP_OK);
    link_state_t state = {.rate_label = "--"};

    // A zero register starts equal to its reset value, but the first write still decodes.
    write_pair(0x11, 0x00);
    assert(link_poll(&state));
    assert(state.rate_hz == 48000);
    assert(strcmp(state.rate_label, "PCM 48k") == 0);
    assert(!link_poll(&state));

    // An identical later write need not refresh, while a real change must refresh.
    write_pair(0x11, 0x00);
    assert(!link_poll(&state));
    write_pair(0x11, 0x08);
    assert(link_poll(&state));
    assert(state.rate_hz == 96000);
    assert(strcmp(state.rate_label, "PCM 96k") == 0);

    // The notify hook runs once per queued write and its wake-up result reaches the ISR's return.
    link_set_notify(notify_stub);
    uint8_t bytes[2] = {0x11, 0x18};
    i2c_slave_rx_done_event_data_t event = {.buffer = bytes, .length = sizeof(bytes)};
    assert(on_receive(NULL, &event, NULL) == true);
    assert(notify_calls == 1);
    assert(link_poll(&state));
    assert(state.rate_hz == 192000);
    link_set_notify(NULL);

    free(s_queue);
    puts("link regression tests passed");
    return 0;
}
