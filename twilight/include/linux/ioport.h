#pragma once

#include <linux/io.h>

struct resource {
    resource_size_t start;
    resource_size_t end;
    const char *name;
    unsigned long flags;
};

static inline resource_size_t resource_size(const struct resource *resource) {
    if (resource == 0 || resource->end < resource->start) return 0;
    return resource->end - resource->start + 1u;
}
