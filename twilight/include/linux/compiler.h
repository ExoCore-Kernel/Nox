#pragma once

#define __iomem
#define __user
#define __force
#define __bitwise
#define __must_check __attribute__((warn_unused_result))
#define __deprecated __attribute__((deprecated))
#define __always_inline inline __attribute__((always_inline))
#define noinline __attribute__((noinline))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define barrier() __asm__ volatile ("" ::: "memory")
