#pragma once

#include <stdint.h>

typedef struct mutex {
    volatile uint32_t value;
} mutex;

#define DEFINE_MUTEX(name) struct mutex name = { .value = 0u }

static inline void mutex_init(struct mutex *lock) {
    if (lock != 0) __atomic_store_n(&lock->value, 0u, __ATOMIC_RELAXED);
}

static inline void mutex_lock(struct mutex *lock) {
    while (__atomic_exchange_n(&lock->value, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
}

static inline int mutex_trylock(struct mutex *lock) {
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&lock->value,
                                       &expected,
                                       1u,
                                       0,
                                       __ATOMIC_ACQUIRE,
                                       __ATOMIC_RELAXED);
}

static inline void mutex_unlock(struct mutex *lock) {
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}
