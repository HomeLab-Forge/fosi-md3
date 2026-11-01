#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define LOGBUF_SIZE 8192

static char s_ring[LOGBUF_SIZE];
static size_t s_head;   // next write position
static bool s_wrapped;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev;

static int tee_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n >= (int)sizeof(line)) {
            n = sizeof(line) - 1;
        }
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < n; i++) {
            s_ring[s_head++] = line[i];
            if (s_head == LOGBUF_SIZE) {
                s_head = 0;
                s_wrapped = true;
            }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return s_prev ? s_prev(fmt, args) : vprintf(fmt, args);
}

void logbuf_init(void)
{
    s_prev = esp_log_set_vprintf(tee_vprintf);
}

size_t logbuf_read(char *out, size_t cap)
{
    if (cap == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    const size_t avail = s_wrapped ? LOGBUF_SIZE : s_head;
    const size_t n = avail < cap - 1 ? avail : cap - 1;
    const size_t start = (s_head + LOGBUF_SIZE - n) % LOGBUF_SIZE;
    for (size_t i = 0; i < n; i++) {
        out[i] = s_ring[(start + i) % LOGBUF_SIZE];
    }
    portEXIT_CRITICAL(&s_lock);
    out[n] = '\0';
    return n;
}
