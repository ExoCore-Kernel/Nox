#pragma once

#include <linux/kernel.h>
#include <linux/types.h>

/* Twilight currently targets little-endian x86_64.  Keep the Linux-facing
 * names separate from libc so unmodified kernel drivers can use the same
 * byte-order helpers they expect in-tree. */
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN    4321
#define __BYTE_ORDER    __LITTLE_ENDIAN
#define __LITTLE_ENDIAN_BITFIELD

#define __constant_cpu_to_le16(x) ((__le16)(u16)(x))
#define __constant_cpu_to_le32(x) ((__le32)(u32)(x))
#define __constant_cpu_to_le64(x) ((__le64)(u64)(x))
#define __constant_le16_to_cpu(x) ((u16)(__le16)(x))
#define __constant_le32_to_cpu(x) ((u32)(__le32)(x))
#define __constant_le64_to_cpu(x) ((u64)(__le64)(x))

#define __constant_cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#define __constant_cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#define __constant_cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#define __constant_be16_to_cpu(x) ((u16)__builtin_bswap16((u16)(x)))
#define __constant_be32_to_cpu(x) ((u32)__builtin_bswap32((u32)(x)))
#define __constant_be64_to_cpu(x) ((u64)__builtin_bswap64((u64)(x)))

#ifndef cpu_to_le16
#define cpu_to_le16(x) ((__le16)(u16)(x))
#endif
#ifndef cpu_to_le32
#define cpu_to_le32(x) ((__le32)(u32)(x))
#endif
#ifndef cpu_to_le64
#define cpu_to_le64(x) ((__le64)(u64)(x))
#endif
#ifndef le16_to_cpu
#define le16_to_cpu(x) ((u16)(__le16)(x))
#endif
#ifndef le32_to_cpu
#define le32_to_cpu(x) ((u32)(__le32)(x))
#endif
#ifndef le64_to_cpu
#define le64_to_cpu(x) ((u64)(__le64)(x))
#endif

#ifndef cpu_to_be16
#define cpu_to_be16(x) ((__be16)__builtin_bswap16((u16)(x)))
#endif
#ifndef cpu_to_be32
#define cpu_to_be32(x) ((__be32)__builtin_bswap32((u32)(x)))
#endif
#ifndef cpu_to_be64
#define cpu_to_be64(x) ((__be64)__builtin_bswap64((u64)(x)))
#endif
#ifndef be16_to_cpu
#define be16_to_cpu(x) ((u16)__builtin_bswap16((u16)(x)))
#endif
#ifndef be32_to_cpu
#define be32_to_cpu(x) ((u32)__builtin_bswap32((u32)(x)))
#endif
#ifndef be64_to_cpu
#define be64_to_cpu(x) ((u64)__builtin_bswap64((u64)(x)))
#endif

#define htons(x) cpu_to_be16(x)
#define ntohs(x) be16_to_cpu(x)
#define htonl(x) cpu_to_be32(x)
#define ntohl(x) be32_to_cpu(x)
