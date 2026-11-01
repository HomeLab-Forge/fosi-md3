#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "audio.c"

static int begin_calls;
static int finish_calls;
static int mount_calls;
static bool last_uac;
static bool last_audio_route;

md3_settings_t g_settings;

void board_audio_power_begin(void)
{
    begin_calls++;
}

void board_uac_select(bool uac1)
{
    last_uac = uac1;
}

void board_usb_mux_set_audio(bool audio)
{
    last_audio_route = audio;
}

void board_audio_power_finish(void)
{
    finish_calls++;
}

esp_err_t dac_init(void)
{
    return ESP_OK;
}

esp_err_t storage_mount(void)
{
    mount_calls++;
    return ESP_OK;
}

int main(void)
{
    g_settings.uac1 = 1;
    g_settings.udisk_mode = 1;
    assert(audio_power_cycle() == ESP_OK);
    assert(begin_calls == 1 && finish_calls == 1);
    assert(last_uac && !last_audio_route);
    assert(mount_calls == 0);

    g_settings.udisk_mode = 0;
    assert(audio_power_cycle() == ESP_OK);
    assert(begin_calls == 2 && finish_calls == 2);
    assert(last_audio_route);
    assert(mount_calls == 1);

    puts("audio mode transition tests passed");
    return 0;
}
