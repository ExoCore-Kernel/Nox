#pragma once

#include <stdint.h>

#include <twilight/timer.h>

#define HZ 1000ul
#define jiffies ((unsigned long)timer_ticks())

static inline unsigned long msecs_to_jiffies(unsigned int m) {
    return (unsigned long)m;
}

static inline unsigned int jiffies_to_msecs(unsigned long j) {
    return (unsigned int)j;
}

#define time_after(a, b) ((long)((b) - (a)) < 0)
#define time_before(a, b) time_after((b), (a))
#define time_after_eq(a, b) ((long)((a) - (b)) >= 0)
#define time_before_eq(a, b) time_after_eq((b), (a))
