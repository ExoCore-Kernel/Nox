#pragma once

#include <stddef.h>

static inline void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    const unsigned char *in = (const unsigned char *)source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}

static inline void *memset(void *destination, int value, size_t count) {
    unsigned char *out = (unsigned char *)destination;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value;
    return destination;
}

static inline int memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *left = (const unsigned char *)a;
    const unsigned char *right = (const unsigned char *)b;
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] < right[i] ? -1 : 1;
    }
    return 0;
}

static inline size_t strlen(const char *string) {
    size_t length = 0;
    if (string == 0) return 0;
    while (string[length] != '\0') ++length;
    return length;
}
