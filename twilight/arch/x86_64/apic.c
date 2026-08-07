#include <stdbool.h>
#include <stdint.h>

#include <twilight/apic.h>
#include <twilight/io.h>

#define IA32_APIC_BASE_MSR 0x1bu
#define IA32_APIC_BASE_ENABLE (1ull << 11)

/*
 * Interrupt Mode Configuration Register used by PC-compatible chipsets.
 * Selecting register 0x70 on port 0x22 and clearing bit 0 on port 0x23
 * routes external interrupts to the legacy 8259 PIC instead of the APIC.
 *
 * Limine/QEMU may leave the virtual-wire path in APIC mode. Merely clearing
 * IA32_APIC_BASE.EN is not sufficient in that case: the PIT can be running
 * and the PIC can have IRQ0 unmasked while the CPU never sees vector 0x20.
 */
#define IMCR_SELECT_PORT 0x22u
#define IMCR_DATA_PORT   0x23u
#define IMCR_REGISTER    0x70u
#define IMCR_APIC_ROUTE  0x01u

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

static void route_external_interrupts_to_pic(void) {
    outb(IMCR_SELECT_PORT, IMCR_REGISTER);
    io_wait();

    const uint8_t value = inb(IMCR_DATA_PORT);
    outb(IMCR_DATA_PORT, (uint8_t)(value & (uint8_t)~IMCR_APIC_ROUTE));
    io_wait();
}

bool apic_disable_for_legacy_pic(void) {
    uint32_t eax, ebx, ecx, edx;
    cpuid(1u, &eax, &ebx, &ecx, &edx);

    /* CPUID.01H:EDX.APIC[9] */
    if ((edx & (1u << 9)) == 0) {
        return false;
    }

    /*
     * First restore the PC chipset's external interrupt route to the 8259s,
     * then turn off the Local APIC. This ordering avoids briefly leaving
     * external IRQs with no receiver on QEMU implementations that honour IMCR.
     */
    route_external_interrupts_to_pic();

    uint64_t base = rdmsr(IA32_APIC_BASE_MSR);
    base &= ~IA32_APIC_BASE_ENABLE;
    wrmsr(IA32_APIC_BASE_MSR, base);
    return true;
}
