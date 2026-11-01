#include "md3.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static md3_snapshot_t s_snapshot;

md3_net_t g_net;

void md3_state_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&g_net, 0, sizeof(g_net));
    strcpy(s_snapshot.screen, "main");
    strcpy(s_snapshot.link.rate_label, "--");
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_state_publish(const md3_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_state_lock);
    const md3_net_t current_net = g_net;
    s_snapshot = *snapshot;
    s_snapshot.net = current_net;
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_state_read(md3_snapshot_t *out)
{
    portENTER_CRITICAL(&s_state_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_net_set(const md3_net_t *state)
{
    portENTER_CRITICAL(&s_state_lock);
    g_net = *state;
    s_snapshot.net = *state;
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_net_get(md3_net_t *out)
{
    portENTER_CRITICAL(&s_state_lock);
    *out = g_net;
    portEXIT_CRITICAL(&s_state_lock);
}
