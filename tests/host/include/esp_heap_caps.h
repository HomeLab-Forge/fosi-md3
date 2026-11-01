#pragma once

#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0

static inline void *heap_caps_malloc(size_t size, unsigned int flags)
{
    (void)flags;
    return malloc(size);
}
