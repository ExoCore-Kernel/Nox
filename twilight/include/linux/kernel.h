#pragma once

#include <stddef.h>
#include <stdint.h>

#include <linux/string.h>
#include <linux/types.h>

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

#define ERR_PTR(error) ((void *)(intptr_t)(error))
#define PTR_ERR(pointer) ((long)(intptr_t)(pointer))
#define IS_ERR(pointer) ((uintptr_t)(pointer) >= (uintptr_t)-4095)
#define IS_ERR_OR_NULL(pointer) ((pointer) == 0 || IS_ERR(pointer))

static inline u16 __twilight_bswap16(u16 value) {
    return (u16)((value << 8) | (value >> 8));
}
static inline u32 __twilight_bswap32(u32 value) {
    return __builtin_bswap32(value);
}

/* Twilight's current x86_64 target is little-endian. */
#define cpu_to_le16(value) ((u16)(value))
#define le16_to_cpu(value) ((u16)(value))
#define cpu_to_le32(value) ((u32)(value))
#define le32_to_cpu(value) ((u32)(value))
#define cpu_to_le64(value) ((u64)(value))
#define le64_to_cpu(value) ((u64)(value))
#define cpu_to_be16(value) __twilight_bswap16((u16)(value))
#define be16_to_cpu(value) __twilight_bswap16((u16)(value))
#define cpu_to_be32(value) __twilight_bswap32((u32)(value))
#define be32_to_cpu(value) __twilight_bswap32((u32)(value))

static inline char *print_mac(char *buffer, const u8 *address) {
    static const char hex[] = "0123456789abcdef";
    if (buffer == 0 || address == 0) return buffer;
    for (unsigned int i = 0; i < 6u; ++i) {
        buffer[i * 3u] = hex[(address[i] >> 4) & 0xfu];
        buffer[i * 3u + 1u] = hex[address[i] & 0xfu];
        if (i != 5u) buffer[i * 3u + 2u] = ':';
    }
    buffer[17] = '\0';
    return buffer;
}

/* Enough for the driver's low-memory warning path; a real token-bucket rate
 * limiter can replace this when the logging subsystem grows one. */
static inline int net_ratelimit(void) { return 1; }
