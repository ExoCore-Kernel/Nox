#pragma once

#include <linux/kernel.h>
#include <linux/types.h>

static inline int test_bit(unsigned long bit, const volatile unsigned long *address) {
    const unsigned long bits_per_word = sizeof(unsigned long) * 8ul;
    return (address[bit / bits_per_word] & (1ul << (bit % bits_per_word))) != 0;
}

static inline void set_bit(unsigned long bit, volatile unsigned long *address) {
    const unsigned long bits_per_word = sizeof(unsigned long) * 8ul;
    __atomic_fetch_or(&address[bit / bits_per_word],
                      1ul << (bit % bits_per_word),
                      __ATOMIC_SEQ_CST);
}

static inline void clear_bit(unsigned long bit, volatile unsigned long *address) {
    const unsigned long bits_per_word = sizeof(unsigned long) * 8ul;
    __atomic_fetch_and(&address[bit / bits_per_word],
                       ~(1ul << (bit % bits_per_word)),
                       __ATOMIC_SEQ_CST);
}

static inline int test_and_set_bit(unsigned long bit, volatile unsigned long *address) {
    const unsigned long bits_per_word = sizeof(unsigned long) * 8ul;
    const unsigned long mask = 1ul << (bit % bits_per_word);
    return (__atomic_fetch_or(&address[bit / bits_per_word], mask, __ATOMIC_SEQ_CST) & mask) != 0;
}
