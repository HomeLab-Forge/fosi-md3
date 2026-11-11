// Keeps the most recent log output in RAM so the USB LOG request can serve it.
#pragma once

#include <stddef.h>

void logbuf_init(void);

/** Copy up to cap-1 bytes of the newest log text into out (NUL-terminated); returns length. */
size_t logbuf_read(char *out, size_t cap);
