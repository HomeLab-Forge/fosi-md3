#include "md3.h"

#include <string.h>

#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static md3_snapshot_t s_snapshot;

void md3_state_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    strcpy(s_snapshot.screen, "main");
    strcpy(s_snapshot.link.rate_label, "--");
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_state_publish(const md3_snapshot_t *snapshot)
{
    portENTER_CRITICAL(&s_state_lock);
    s_snapshot = *snapshot;
    portEXIT_CRITICAL(&s_state_lock);
}

void md3_state_read(md3_snapshot_t *out)
{
    portENTER_CRITICAL(&s_state_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_state_lock);
}
