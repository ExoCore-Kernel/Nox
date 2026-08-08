#pragma once

#include <stdint.h>

#include <twilight/timer.h>

static inline void msleep(unsigned int milliseconds) {
    timer_sleep_ms(milliseconds);
}

static inline void ssleep(unsigned int seconds) {
    timer_sleep_ms((uint64_t)seconds * 1000ull);
}

static inline void mdelay(unsigned long milliseconds) {
    timer_sleep_ms((uint64_t)milliseconds);
}

static inline void udelay(unsigned long microseconds) {
    /* Sub-millisecond calibration will move to TSC/HPET later. This bounded
     * pause loop is sufficient for current QEMU device register settling. */
    volatile unsigned long iterations = microseconds * 128ul + 1ul;
    while (iterations-- != 0ul) __asm__ volatile ("pause");
}

static inline void ndelay(unsigned long nanoseconds) {
    udelay((nanoseconds + 999ul) / 1000ul);
}
