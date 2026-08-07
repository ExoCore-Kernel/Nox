#pragma once

#include <stdint.h>

typedef struct {
    volatile int counter;
} atomic_t;

#define ATOMIC_INIT(value) { .counter = (value) }

static inline int atomic_read(const atomic_t *value) {
    return __atomic_load_n(&value->counter, __ATOMIC_RELAXED);
}

static inline void atomic_set(atomic_t *value, int new_value) {
    __atomic_store_n(&value->counter, new_value, __ATOMIC_RELAXED);
}

static inline void atomic_inc(atomic_t *value) {
    (void)__atomic_add_fetch(&value->counter, 1, __ATOMIC_ACQ_REL);
}

static inline void atomic_dec(atomic_t *value) {
    (void)__atomic_sub_fetch(&value->counter, 1, __ATOMIC_ACQ_REL);
}

static inline int atomic_inc_return(atomic_t *value) {
    return __atomic_add_fetch(&value->counter, 1, __ATOMIC_ACQ_REL);
}

static inline int atomic_dec_return(atomic_t *value) {
    return __atomic_sub_fetch(&value->counter, 1, __ATOMIC_ACQ_REL);
}

static inline int atomic_dec_and_test(atomic_t *value) {
    return atomic_dec_return(value) == 0;
}

static inline int atomic_cmpxchg(atomic_t *value, int old_value, int new_value) {
    __atomic_compare_exchange_n(&value->counter,
                                &old_value,
                                new_value,
                                0,
                                __ATOMIC_ACQ_REL,
                                __ATOMIC_RELAXED);
    return old_value;
}
