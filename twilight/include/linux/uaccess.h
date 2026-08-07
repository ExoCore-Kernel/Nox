#pragma once

#include <stddef.h>
#include <linux/string.h>

#define __user

static inline unsigned long copy_to_user(void *to, const void *from, unsigned long n) {
    if (to == 0 || from == 0) return n;
    memcpy(to, from, (size_t)n);
    return 0;
}

static inline unsigned long copy_from_user(void *to, const void *from, unsigned long n) {
    if (to == 0 || from == 0) return n;
    memcpy(to, from, (size_t)n);
    return 0;
}
