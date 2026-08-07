#pragma once

#include <stdint.h>

#include <linux/interrupt.h>

typedef struct {
    volatile uint32_t value;
} spinlock_t;

#define __SPIN_LOCK_UNLOCKED(name) { .value = 0u }
#define DEFINE_SPINLOCK(name) spinlock_t name = __SPIN_LOCK_UNLOCKED(name)

static inline void spin_lock_init(spinlock_t *lock) {
    if (lock != 0) __atomic_store_n(&lock->value, 0u, __ATOMIC_RELAXED);
}

static inline void spin_lock(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->value, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
}

static inline int spin_trylock(spinlock_t *lock) {
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&lock->value, &expected, 1u, 0,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static inline void spin_unlock(spinlock_t *lock) {
    __atomic_store_n(&lock->value, 0u, __ATOMIC_RELEASE);
}

#define spin_lock_irqsave(lock, flags) do { \
    local_irq_save(flags); \
    spin_lock(lock); \
} while (0)

#define spin_unlock_irqrestore(lock, flags) do { \
    spin_unlock(lock); \
    local_irq_restore(flags); \
} while (0)

#define spin_lock_irq(lock) do { local_irq_disable(); spin_lock(lock); } while (0)
#define spin_unlock_irq(lock) do { spin_unlock(lock); local_irq_enable(); } while (0)

/* Twilight currently has no separate softirq execution context. NAPI is
 * deferred by the driver runtime, so BH exclusion reduces to the lock itself. */
#define spin_lock_bh(lock) spin_lock(lock)
#define spin_unlock_bh(lock) spin_unlock(lock)
