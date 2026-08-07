#pragma once

#include <stddef.h>
#include <stdint.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#define BIT(nr) (1ull << (nr))
#define BIT_ULL(nr) (1ull << (nr))

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define min(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#define max(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a > _b ? _a : _b; })
#define clamp(v, lo, hi) min(max((v), (lo)), (hi))

#define ALIGN(value, alignment) \
    (((value) + (__typeof__(value))(alignment) - 1) & ~((__typeof__(value))(alignment) - 1))
#define IS_ALIGNED(value, alignment) (((value) & ((alignment) - 1)) == 0)

#define container_of(pointer, type, member) ({                              \
    const __typeof__(((type *)0)->member) *_member_pointer = (pointer);      \
    (type *)((char *)_member_pointer - offsetof(type, member));              \
})

#define READ_ONCE(value) (*(volatile __typeof__(value) *)&(value))
#define WRITE_ONCE(value, new_value) do {                                   \
    (*(volatile __typeof__(value) *)&(value)) = (new_value);                 \
} while (0)

#define barrier() __asm__ volatile ("" ::: "memory")
