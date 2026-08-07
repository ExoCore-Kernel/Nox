#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/cpu_security.h>
#include <twilight/gdt.h>

struct __attribute__((packed)) x86_64_tss {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
};

struct __attribute__((packed)) gdtr {
    uint16_t limit;
    uint64_t base;
};

struct x86_64_tss gdt_tss;

static uint64_t gdt[7] __attribute__((aligned(16)));
static bool initialized;

extern void twilight_gdt_load(const struct gdtr *descriptor);

static void bytes_zero(void *pointer, size_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xffffu);
    low |= (base & 0xffffffull) << 16;
    low |= 0x89ull << 40;
    low |= (uint64_t)((limit >> 16) & 0x0fu) << 48;
    low |= ((base >> 24) & 0xffull) << 56;

    gdt[5] = low;
    gdt[6] = base >> 32;
}

bool gdt_init(void) {
    initialized = false;
    bytes_zero(gdt, sizeof(gdt));
    bytes_zero(&gdt_tss, sizeof(gdt_tss));

    gdt[0] = 0x0000000000000000ull;
    gdt[1] = 0x00af9a000000ffffull;
    gdt[2] = 0x00cf92000000ffffull;
    gdt[3] = 0x00cff2000000ffffull;
    gdt[4] = 0x00affa000000ffffull;

    /* No I/O bitmap is present inside the TSS limit, so CPL3 has no direct
     * port-I/O permission. Drivers remain mediated by the kernel. */
    gdt_tss.iomap_base = (uint16_t)sizeof(gdt_tss);
    set_tss_descriptor((uint64_t)(uintptr_t)&gdt_tss,
                       (uint32_t)(sizeof(gdt_tss) - 1u));

    const struct gdtr descriptor = {
        .limit = (uint16_t)(sizeof(gdt) - 1u),
        .base = (uint64_t)(uintptr_t)gdt,
    };

    twilight_gdt_load(&descriptor);

    uint16_t cs = 0;
    uint16_t tr = 0;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile ("str %0" : "=r"(tr));

    if (cs != TWILIGHT_GDT_KERNEL_CODE || tr != TWILIGHT_GDT_TSS) return false;
    if (!cpu_security_init()) return false;

    initialized = true;
    return true;
}

bool gdt_is_initialized(void) {
    return initialized;
}

uint64_t gdt_kernel_stack(void) {
    return gdt_tss.rsp0;
}
