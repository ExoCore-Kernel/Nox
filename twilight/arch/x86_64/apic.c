#include <stdbool.h>
#include <stdint.h>

#include <twilight/apic.h>
#include <twilight/io.h>
#include <twilight/mmio.h>
#include <twilight/pmm.h>

#define IA32_APIC_BASE_MSR 0x1bu
#define IA32_APIC_BASE_ENABLE (1ull << 11)
#define IA32_APIC_BASE_X2APIC (1ull << 10)
#define IA32_APIC_BASE_ADDRESS_MASK 0xfffff000ull

#define LAPIC_ID        0x020u
#define LAPIC_EOI       0x0b0u
#define LAPIC_SVR       0x0f0u
#define LAPIC_LVT_LINT0 0x350u
#define LAPIC_LVT_LINT1 0x360u

#define LAPIC_SVR_ENABLE          (1u << 8)
#define LAPIC_LVT_DELIVERY_MASK   (7u << 8)
#define LAPIC_LVT_DELIVERY_EXTINT (7u << 8)
#define LAPIC_LVT_MASKED          (1u << 16)

#define IMCR_SELECT_PORT 0x22u
#define IMCR_DATA_PORT   0x23u
#define IMCR_REGISTER    0x70u
#define IMCR_APIC_ROUTE  0x01u

static volatile uint32_t *lapic_mmio;
static bool virtual_wire_enabled;
static bool native_enabled;

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx) {
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
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi) : "memory");
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

static bool cpu_has_apic(void) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid(1u, &eax, &ebx, &ecx, &edx);
    (void)eax;
    (void)ebx;
    (void)ecx;
    return (edx & (1u << 9)) != 0;
}

static uint32_t lapic_read(uint32_t offset) {
    return lapic_mmio[offset / 4u];
}

static void lapic_write(uint32_t offset, uint32_t value) {
    lapic_mmio[offset / 4u] = value;
    (void)lapic_mmio[LAPIC_SVR / 4u];
}

static bool map_and_enable_xapic(void) {
    if (!cpu_has_apic()) return false;

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & IA32_APIC_BASE_X2APIC) != 0) {
        /* Twilight's current interrupt core targets conventional MMIO xAPIC.
         * Leave x2APIC atomically before enabling xAPIC mode. */
        base &= ~(IA32_APIC_BASE_ENABLE | IA32_APIC_BASE_X2APIC);
        wrmsr(IA32_APIC_BASE_MSR, base);
        base = rdmsr(IA32_APIC_BASE_MSR);
    }

    base &= ~IA32_APIC_BASE_X2APIC;
    base |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    const uint64_t physical = base & IA32_APIC_BASE_ADDRESS_MASK;
    if (physical == 0) return false;

    /* Prefer the cache-disabled MMIO mapper once it is available. The HHDM
     * fallback keeps very early/diagnostic use possible. */
    if (mmio_is_initialized())
        lapic_mmio = (volatile uint32_t *)mmio_map(physical, 4096u);
    if (lapic_mmio == 0)
        lapic_mmio = (volatile uint32_t *)pmm_phys_to_virt(physical);
    if (lapic_mmio == 0) return false;

    uint32_t svr = lapic_read(LAPIC_SVR);
    if ((svr & 0xffu) < 0x10u) svr = (svr & ~0xffu) | 0xffu;
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);
    return true;
}

bool apic_disable_for_legacy_pic(void) {
    virtual_wire_enabled = false;
    native_enabled = false;
    lapic_mmio = 0;

    if (!cpu_has_apic()) return false;

    set_imcr_route(false);

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    if ((base & IA32_APIC_BASE_X2APIC) != 0) {
        base &= ~(IA32_APIC_BASE_ENABLE | IA32_APIC_BASE_X2APIC);
        wrmsr(IA32_APIC_BASE_MSR, base);
        return true;
    }

    base &= ~IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);
    return true;
}

bool apic_enable_virtual_wire_for_legacy_pic(void) {
    native_enabled = false;
    virtual_wire_enabled = false;
    if (!map_and_enable_xapic()) return false;

    uint32_t lint0 = lapic_read(LAPIC_LVT_LINT0);
    lint0 &= ~LAPIC_LVT_DELIVERY_MASK;
    lint0 &= ~LAPIC_LVT_MASKED;
    lint0 |= LAPIC_LVT_DELIVERY_EXTINT;
    lapic_write(LAPIC_LVT_LINT0, lint0);

    set_imcr_route(true);
    virtual_wire_enabled = true;
    return true;
}

bool apic_enable_native(void) {
    virtual_wire_enabled = false;
    native_enabled = false;
    if (!map_and_enable_xapic()) return false;

    /* IOAPIC/MSI own external interrupt delivery. Do not leave the 8259's
     * ExtINT signal live on LINT0 when native mode is selected. */
    uint32_t lint0 = lapic_read(LAPIC_LVT_LINT0);
    lint0 |= LAPIC_LVT_MASKED;
    lapic_write(LAPIC_LVT_LINT0, lint0);

    uint32_t lint1 = lapic_read(LAPIC_LVT_LINT1);
    lint1 |= LAPIC_LVT_MASKED;
    lapic_write(LAPIC_LVT_LINT1, lint1);

    set_imcr_route(true);
    native_enabled = true;
    return true;
}

bool apic_native_enabled(void) {
    return native_enabled && lapic_mmio != 0;
}

uint8_t apic_id(void) {
    if (lapic_mmio != 0) return (uint8_t)(lapic_read(LAPIC_ID) >> 24);

    uint32_t eax, ebx, ecx, edx;
    cpuid(1u, &eax, &ebx, &ecx, &edx);
    (void)eax; (void)ecx; (void)edx;
    return (uint8_t)(ebx >> 24);
}

void apic_eoi_if_needed(void) {
    if ((!virtual_wire_enabled && !native_enabled) || lapic_mmio == 0) return;
    lapic_write(LAPIC_EOI, 0u);
}
