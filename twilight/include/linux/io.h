#pragma once

#include <stddef.h>
#include <stdint.h>

#include <linux/types.h>
#include <twilight/io.h>
#include <twilight/mmio.h>

#define __iomem
#define __force

typedef uint64_t resource_size_t;

static inline void __iomem *ioremap(phys_addr_t physical, size_t size) {
    return mmio_map((uint64_t)physical, size);
}

static inline void iounmap(void __iomem *address) {
    (void)mmio_unmap(address);
}

static inline u8 readb(const volatile void __iomem *address) { return *(const volatile u8 *)address; }
static inline u16 readw(const volatile void __iomem *address) { return *(const volatile u16 *)address; }
static inline u32 readl(const volatile void __iomem *address) { return *(const volatile u32 *)address; }
static inline u64 readq(const volatile void __iomem *address) { return *(const volatile u64 *)address; }

static inline void writeb(u8 value, volatile void __iomem *address) { *(volatile u8 *)address = value; }
static inline void writew(u16 value, volatile void __iomem *address) { *(volatile u16 *)address = value; }
static inline void writel(u32 value, volatile void __iomem *address) { *(volatile u32 *)address = value; }
static inline void writeq(u64 value, volatile void __iomem *address) { *(volatile u64 *)address = value; }

#define ioread8(address) readb(address)
#define ioread16(address) readw(address)
#define ioread32(address) readl(address)
#define iowrite8(value, address) writeb((value), (address))
#define iowrite16(value, address) writew((value), (address))
#define iowrite32(value, address) writel((value), (address))

static inline void memcpy_fromio(void *destination,
                                 const volatile void __iomem *source,
                                 size_t count) {
    u8 *out = (u8 *)destination;
    const volatile u8 *in = (const volatile u8 *)source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
}

static inline void mb(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
static inline void rmb(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
static inline void wmb(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
