#include <stdbool.h>
#include <stdint.h>

#include <twilight/apic.h>
#include <twilight/io.h>

#define IA32_APIC_BASE_MSR 0x1bu
#define IA32_APIC_BASE_ENABLE (1ull << 11)
#define IA32_APIC_BASE_X2APIC (1ull << 10)

/* x2APIC MSR numbers are 0x800 + (xAPIC register offset >> 4). */
#define X2APIC_EOI_MSR       0x80bu
#define X2APIC_SVR_MSR       0x80fu
#define X2APIC_LVT_LINT0_MSR 0x835u

#define LAPIC_SVR_ENABLE          (1u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT (7u << 8)
#define LAPIC_LVT_MASKED          (1u << 16)

/*
 * Interrupt Mode Configuration Register used by PC-compatible chipsets.
 * Selecting register 0x70 on port 0x22 and changing bit 0 chooses whether
 * external 8259 interrupts go directly to the processor or through the APIC.
 */
#define IMCR_SELECT_PORT 0x22u
#define IMCR_DATA_PORT   0x23u
#define IMCR_REGISTER    0x70u
#define IMCR_APIC_ROUTE  0x01u

static bool virtual_wire_enabled;
static bool virtual_wire_uses_x2apic;

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(0)
    );
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    const uint32_t lo = (uint32_t)value;
    const uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static void set_imcr_route(bool apic_route) {
    outb(IMCR_SELECT_PORT, IMCR_REGISTER);
    io_wait();

    uint8_t value = inb(IMCR_DATA_PORT);
    if (apic_route) value |= IMCR_APIC_ROUTE;
    else value &= (uint8_t)~IMCR_APIC_ROUTE;

    outb(IMCR_DATA_PORT, value);
    io_wait();
}

static void cpuid_leaf1(uint32_t *ecx, uint32_t *edx) {
    uint32_t eax;
    uint32_t ebx;
    cpuid(1u, &eax, &ebx, ecx, edx);
}

static bool cpu_has_apic(void) {
    uint32_t ecx;
    uint32_t edx;
    cpuid_leaf1(&ecx, &edx);
    (void)ecx;
    return (edx & (1u << 9)) != 0;
}

static bool cpu_has_x2apic(void) {
    uint32_t ecx;
    uint32_t edx;
    cpuid_leaf1(&ecx, &edx);
    (void)edx;
    return (ecx & (1u << 21)) != 0;
}

bool apic_disable_for_legacy_pic(void) {
    virtual_wire_enabled = false;
    virtual_wire_uses_x2apic = false;

    if (!cpu_has_apic()) return false;

    /* Try the simplest legacy path first: PIC output directly to CPU INTR. */
    set_imcr_route(false);

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);

    /* If x2APIC was active, leave x2APIC before disabling the APIC globally. */
    if ((base & IA32_APIC_BASE_X2APIC) != 0) {
        base &= ~IA32_APIC_BASE_X2APIC;
        wrmsr(IA32_APIC_BASE_MSR, base);
    }

    base &= ~IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);
    return true;
}

bool apic_enable_virtual_wire_for_legacy_pic(void) {
    if (!cpu_has_apic() || !cpu_has_x2apic()) return false;

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);

    /*
     * Enter x2APIC through the architecturally valid state sequence:
     * disabled -> xAPIC enabled -> x2APIC enabled.
     *
     * Using x2APIC MSRs avoids assuming that the LAPIC MMIO page at
     * 0xFEE00000 is present in Limine's RAM-oriented HHDM mapping.
     */
    base &= ~IA32_APIC_BASE_X2APIC;
    base |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    base |= IA32_APIC_BASE_X2APIC;
    wrmsr(IA32_APIC_BASE_MSR, base);

    /* Enable the Local APIC and ensure a valid spurious interrupt vector. */
    uint32_t svr = (uint32_t)rdmsr(X2APIC_SVR_MSR);
    if ((svr & 0xffu) < 0x10u) {
        svr = (svr & ~0xffu) | 0xffu;
    }
    svr |= LAPIC_SVR_ENABLE;
    wrmsr(X2APIC_SVR_MSR, svr);

    /* LINT0 receives the legacy 8259 INTR signal as ExtINT, unmasked. */
    uint32_t lint0 = (uint32_t)rdmsr(X2APIC_LVT_LINT0_MSR);
    lint0 &= ~(7u << 8);
    lint0 &= ~LAPIC_LVT_MASKED;
    lint0 |= LAPIC_LVT_DELIVERY_EXTINT;
    wrmsr(X2APIC_LVT_LINT0_MSR, lint0);

    /* Now route the chipset's legacy interrupt output into the Local APIC. */
    set_imcr_route(true);

    virtual_wire_uses_x2apic = true;
    virtual_wire_enabled = true;
    return true;
}

void apic_eoi_if_needed(void) {
    if (!virtual_wire_enabled) return;

    if (virtual_wire_uses_x2apic) {
        wrmsr(X2APIC_EOI_MSR, 0);
    }
}
