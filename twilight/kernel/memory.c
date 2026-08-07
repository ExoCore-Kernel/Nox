#include <stddef.h>

/*
 * Freestanding memory primitives.
 *
 * The volatile accesses are deliberate: without them an optimizing compiler
 * can recognize these loops and lower them back into calls to memset/memcpy,
 * recursively calling the function we are currently implementing.
 */
void *memset(void *dest, int value, size_t count) {
    volatile unsigned char *d = (volatile unsigned char *)dest;
    for (size_t i = 0; i < count; ++i) {
        d[i] = (unsigned char)value;
    }
    return dest;
}

void *memcpy(void *dest, const void *src, size_t count) {
    volatile unsigned char *d = (volatile unsigned char *)dest;
    const volatile unsigned char *s = (const volatile unsigned char *)src;
    for (size_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t count) {
    volatile unsigned char *d = (volatile unsigned char *)dest;
    const volatile unsigned char *s = (const volatile unsigned char *)src;

    if (d == s || count == 0) {
        return dest;
    }

    if ((const void *)d < (const void *)s) {
        for (size_t i = 0; i < count; ++i) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = count; i > 0; --i) {
            d[i - 1] = s[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *a, const void *b, size_t count) {
    const volatile unsigned char *aa = (const volatile unsigned char *)a;
    const volatile unsigned char *bb = (const volatile unsigned char *)b;

    for (size_t i = 0; i < count; ++i) {
        if (aa[i] != bb[i]) {
            return (int)aa[i] - (int)bb[i];
        }
    }

    return 0;
}
