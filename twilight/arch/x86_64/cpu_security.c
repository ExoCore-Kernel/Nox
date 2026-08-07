#include <stdbool.h>
#include <stdint.h>

#include <twilight/cpu_security.h>
#include <twilight/log.h>

#define CR0_WRITE_PROTECT (1ull << 16)
#define CR4_PCE           (1ull << 8)
#define CR4_UMIP          (1ull << 11)
#define CR4_SMEP          (1ull << 20)
#define CR4_SMAP          (1ull << 21)

static struct cpu_security_status status;
static bool initialized;

static void cpuid(uint32_t leaf,
                  uint32_t subleaf,
                  uint32_t *eax,
                  uint32_t *ebx,
                  uint32_t *ecx,
                  uint32_t *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

static uint64_t read_cr0(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(value));
    return value;
}

static void write_cr0(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr0" : : "r"(value) : "memory");
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr4(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr4" : : "r"(value) : "memory");
}

bool cpu_security_init(void) {
    status = (struct cpu_security_status){0};
    initialized = false;

    uint64_t cr0 = read_cr0();
    cr0 |= CR0_WRITE_PROTECT;
    write_cr0(cr0);
    status.write_protect = (read_cr0() & CR0_WRITE_PROTECT) != 0;
    if (!status.write_protect) return false;

    uint32_t max_leaf = 0;
    uint32_t ignored_b = 0;
    uint32_t ignored_c = 0;
    uint32_t ignored_d = 0;
    cpuid(0, 0, &max_leaf, &ignored_b, &ignored_c, &ignored_d);

    uint64_t cr4 = read_cr4();
    cr4 &= ~CR4_PCE;

    if (max_leaf >= 7u) {
        uint32_t eax, ebx, ecx, edx;
        cpuid(7u, 0u, &eax, &ebx, &ecx, &edx);
        (void)eax;
        (void)edx;

        if ((ebx & (1u << 7)) != 0) cr4 |= CR4_SMEP;
        if ((ebx & (1u << 20)) != 0) cr4 |= CR4_SMAP;
        if ((ecx & (1u << 2)) != 0) cr4 |= CR4_UMIP;
    }

    write_cr4(cr4);
    const uint64_t active = read_cr4();
    status.smep = (active & CR4_SMEP) != 0;
    status.smap = (active & CR4_SMAP) != 0;
    status.umip = (active & CR4_UMIP) != 0;

    initialized = true;
    klog("CPU security: CR0.WP enabled; user RDPMC disabled");
    if (status.smep) klog("CPU security: SMEP enabled (kernel cannot execute user pages)");
    else klog("CPU security: SMEP unavailable on this CPU");
    if (status.smap) klog("CPU security: SMAP enabled (kernel user-memory access requires explicit override)");
    else klog("CPU security: SMAP unavailable on this CPU");
    if (status.umip) klog("CPU security: UMIP enabled (sensitive descriptor-table instructions blocked in Ring 3)");
    else klog("CPU security: UMIP unavailable on this CPU");
    return true;
}

void cpu_security_get_status(struct cpu_security_status *out) {
    if (out == 0) return;
    if (!initialized) {
        *out = (struct cpu_security_status){0};
        return;
    }
    *out = status;
}

void cpu_user_access_begin(void) {
    if (initialized && status.smap) __asm__ volatile ("stac" : : : "memory");
}

void cpu_user_access_end(void) {
    if (initialized && status.smap) __asm__ volatile ("clac" : : : "memory");
}
