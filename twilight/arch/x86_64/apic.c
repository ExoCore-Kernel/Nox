#include <stdbool.h>
#include <stdint.h>

#include <twilight/apic.h>
#include <twilight/io.h>
#include <twilight/pmm.h>

#define IA32_APIC_BASE_MSR 0x1bu
#define IA32_APIC_BASE_ENABLE (1ull << 11)
#define IA32_APIC_BASE_X2APIC (1ull << 10)
#define IA32_APIC_BASE_ADDRESS_MASK 0xfffff000ull

#define LAPIC_EOI       0x0b0u
#define LAPIC_SVR       0x0f0u
#define LAPIC_LVT_LINT0 0x350u

#define LAPIC_SVR_ENABLE          (1u << 8)
#define LAPIC_LVT_DELIVERY_MASK   (7u << 8)
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

static volatile uint32_t *lapic_mmio;
static bool virtual_wire_enabled;

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
    /* Serialize the posted MMIO write with a read from the same device. */
    (void)lapic_mmio[LAPIC_SVR / 4u];
}

bool apic_disable_for_legacy_pic(void) {
    virtual_wire_enabled = false;
    lapic_mmio = 0;

    if (!cpu_has_apic()) return false;

    /* First try direct legacy PIC -> CPU INTR routing. */
    set_imcr_route(false);

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);

    /*
     * x2APIC cannot transition directly back to xAPIC. If it was enabled by
     * firmware/bootloader, leave x2APIC by disabling both mode bits at once.
     */
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
    if (!cpu_has_apic()) return false;

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);

    /*
     * TCG on non-x86 hosts may not expose x2APIC, but conventional xAPIC is
     * available. Re-enable xAPIC while keeping the x2APIC bit clear.
     */
    base &= ~IA32_APIC_BASE_X2APIC;
    base |= IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);

    const uint64_t lapic_physical = base & IA32_APIC_BASE_ADDRESS_MASK;
    if (lapic_physical == 0) return false;

    /*
     * Limine's HHDM gives Twilight a direct virtual mapping of physical
     * addresses during early boot. Use that mapping for the xAPIC MMIO page.
     */
    lapic_mmio = (volatile uint32_t *)pmm_phys_to_virt(lapic_physical);
    if (lapic_mmio == 0) return false;

    /* Enable the Local APIC and keep a valid spurious interrupt vector. */
    uint32_t svr = lapic_read(LAPIC_SVR);
    if ((svr & 0xffu) < 0x10u) {
        svr = (svr & ~0xffu) | 0xffu;
    }
    svr |= LAPIC_SVR_ENABLE;
    lapic_write(LAPIC_SVR, svr);

    /* LINT0 receives the 8259's INTR signal using ExtINT delivery mode. */
    uint32_t lint0 = lapic_read(LAPIC_LVT_LINT0);
    lint0 &= ~LAPIC_LVT_DELIVERY_MASK;
    lint0 &= ~LAPIC_LVT_MASKED;
    lint0 |= LAPIC_LVT_DELIVERY_EXTINT;
    lapic_write(LAPIC_LVT_LINT0, lint0);

    /* Route the chipset's legacy interrupt output into LINT0. */
    set_imcr_route(true);

    virtual_wire_enabled = true;
    return true;
}

void apic_eoi_if_needed(void) {
    if (!virtual_wire_enabled || lapic_mmio == 0) return;
    lapic_write(LAPIC_EOI, 0u);
}
