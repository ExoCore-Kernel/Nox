#include <stdbool.h>
#include <stdint.h>

#include <twilight/cpu_security.h>
#include <twilight/log.h>

#define CR0_MONITOR_COPROCESSOR (1ull << 1)
#define CR0_EMULATION           (1ull << 2)
#define CR0_TASK_SWITCHED       (1ull << 3)
#define CR0_NUMERIC_ERROR       (1ull << 5)
#define CR0_WRITE_PROTECT       (1ull << 16)

#define CR4_PCE                 (1ull << 8)
#define CR4_OSFXSR              (1ull << 9)
#define CR4_OSXMMEXCPT          (1ull << 10)
#define CR4_UMIP                (1ull << 11)
#define CR4_SMEP                (1ull << 20)
#define CR4_SMAP                (1ull << 21)

#define CPUID1_EDX_FPU          (1u << 0)
#define CPUID1_EDX_FXSR         (1u << 24)
#define CPUID1_EDX_SSE          (1u << 25)
#define CPUID1_EDX_SSE2         (1u << 26)

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

static bool enable_userspace_fpu_sse(uint32_t max_leaf) {
    if (max_leaf < 1u) return false;

    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    cpuid(1u, 0u, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)ecx;

    const uint32_t required = CPUID1_EDX_FPU | CPUID1_EDX_FXSR |
                              CPUID1_EDX_SSE | CPUID1_EDX_SSE2;
    if ((edx & required) != required) return false;

    /* x86-64 Linux userspace is allowed to use SSE/SSE2 without asking the
     * kernel first.  Clear the legacy emulation/task-switched traps, enable
     * native numeric exception handling, and tell the CPU the OS supports
     * FXSAVE/FXRSTOR plus unmasked SIMD exception delivery.
     *
     * Twilight's kernel itself is still compiled -mno-sse/-mno-sse2.  Until a
     * scheduler exists there is only one live userspace FPU context, so no
     * task-to-task FPU save/restore is required yet. */
    uint64_t cr0 = read_cr0();
    cr0 |= CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR;
    cr0 &= ~(CR0_EMULATION | CR0_TASK_SWITCHED);
    write_cr0(cr0);

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    write_cr4(cr4);

    const uint64_t active_cr0 = read_cr0();
    const uint64_t active_cr4 = read_cr4();
    if ((active_cr0 & (CR0_EMULATION | CR0_TASK_SWITCHED)) != 0) return false;
    if ((active_cr0 & (CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR)) !=
        (CR0_MONITOR_COPROCESSOR | CR0_NUMERIC_ERROR)) return false;
    if ((active_cr4 & (CR4_OSFXSR | CR4_OSXMMEXCPT)) !=
        (CR4_OSFXSR | CR4_OSXMMEXCPT)) return false;

    /* Establish the same clean architectural state a fresh Linux task expects:
     * x87 control word 0x037f and MXCSR 0x1f80 (all exceptions masked). */
    const uint32_t mxcsr = 0x1f80u;
    __asm__ volatile ("fninit" ::: "memory");
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr) : "memory");
    return true;
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

    /* Enable the architectural floating-point/SIMD baseline before entering
     * arbitrary x86-64 userspace.  In particular, SSE/SSE2 instructions raise
     * #UD when CR4.OSFXSR is not enabled even if CPUID advertises SSE. */
    status.fpu_sse = enable_userspace_fpu_sse(max_leaf);
    if (!status.fpu_sse) return false;

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
    status.fpu_sse = status.fpu_sse &&
                     (active & (CR4_OSFXSR | CR4_OSXMMEXCPT)) ==
                         (CR4_OSFXSR | CR4_OSXMMEXCPT);
    if (!status.fpu_sse) return false;

    initialized = true;
    klog("CPU security: CR0.WP enabled; user RDPMC disabled");
    klog("CPU userspace: x87 + SSE/SSE2 enabled; clean FPU/MXCSR state initialized");
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
