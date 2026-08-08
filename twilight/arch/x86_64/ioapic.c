#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/acpi.h>
#include <twilight/apic.h>
#include <twilight/io.h>
#include <twilight/ioapic.h>
#include <twilight/mmio.h>

#define MADT_ENTRY_LOCAL_APIC 0u
#define MADT_ENTRY_IOAPIC 1u
#define MADT_ENTRY_INTERRUPT_OVERRIDE 2u

#define IOAPIC_MAX_UNITS 4u
#define LEGACY_IRQ_COUNT 16u

#define IOAPIC_REG_ID      0x00u
#define IOAPIC_REG_VERSION 0x01u
#define IOAPIC_REG_REDTBL  0x10u

#define IOAPIC_REDIR_MASKED       (1ull << 16)
#define IOAPIC_REDIR_POLARITY_LOW (1ull << 13)
#define IOAPIC_REDIR_TRIGGER_LEVEL (1ull << 15)

struct __attribute__((packed)) acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t entries[];
};

struct __attribute__((packed)) madt_entry_header {
    uint8_t type;
    uint8_t length;
};

struct __attribute__((packed)) madt_ioapic_entry {
    struct madt_entry_header header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t address;
    uint32_t gsi_base;
};

struct __attribute__((packed)) madt_interrupt_override {
    struct madt_entry_header header;
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
};

struct ioapic_unit {
    volatile uint32_t *mmio;
    uint32_t gsi_base;
    uint32_t redirection_count;
    uint8_t firmware_id;
};

struct legacy_route {
    uint32_t gsi;
    bool active_low;
    bool level_triggered;
};

static struct ioapic_unit units[IOAPIC_MAX_UNITS];
static size_t unit_count;
static struct legacy_route legacy_routes[LEGACY_IRQ_COUNT];
static bool active;

static uint32_t ioapic_read(const struct ioapic_unit *unit, uint8_t reg) {
    unit->mmio[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    return unit->mmio[4];
}

static void ioapic_write(const struct ioapic_unit *unit, uint8_t reg, uint32_t value) {
    unit->mmio[0] = reg;
    __asm__ volatile ("mfence" ::: "memory");
    unit->mmio[4] = value;
    __asm__ volatile ("mfence" ::: "memory");
}

static struct ioapic_unit *unit_for_gsi(uint32_t gsi, uint32_t *pin_out) {
    for (size_t i = 0; i < unit_count; ++i) {
        struct ioapic_unit *unit = &units[i];
        if (gsi < unit->gsi_base) continue;
        const uint32_t pin = gsi - unit->gsi_base;
        if (pin >= unit->redirection_count) continue;
        if (pin_out != 0) *pin_out = pin;
        return unit;
    }
    return 0;
}

static bool program_legacy_irq(uint8_t irq, bool masked) {
    if (!active || irq >= LEGACY_IRQ_COUNT) return false;

    const struct legacy_route *route = &legacy_routes[irq];
    uint32_t pin = 0;
    struct ioapic_unit *unit = unit_for_gsi(route->gsi, &pin);
    if (unit == 0) return false;

    uint64_t value = (uint64_t)(0x20u + irq);
    if (route->active_low) value |= IOAPIC_REDIR_POLARITY_LOW;
    if (route->level_triggered) value |= IOAPIC_REDIR_TRIGGER_LEVEL;
    if (masked) value |= IOAPIC_REDIR_MASKED;
    value |= (uint64_t)apic_id() << 56;

    const uint8_t low_reg = (uint8_t)(IOAPIC_REG_REDTBL + pin * 2u);
    ioapic_write(unit, (uint8_t)(low_reg + 1u), (uint32_t)(value >> 32));
    ioapic_write(unit, low_reg, (uint32_t)value);
    return true;
}

static void reset_state(void) {
    active = false;
    unit_count = 0;
    for (size_t i = 0; i < IOAPIC_MAX_UNITS; ++i) units[i] = (struct ioapic_unit){0};
    for (uint8_t irq = 0; irq < LEGACY_IRQ_COUNT; ++irq) {
        legacy_routes[irq].gsi = irq;
        legacy_routes[irq].active_low = false;
        legacy_routes[irq].level_triggered = false;
    }
}

bool ioapic_init(void) {
    reset_state();
    if (!acpi_is_initialized() || !mmio_is_initialized()) return false;

    const struct acpi_sdt_header *header = acpi_find_table("APIC");
    if (header == 0 || header->length < sizeof(struct acpi_madt)) return false;
    const struct acpi_madt *madt = (const struct acpi_madt *)header;

    const uint8_t *cursor = madt->entries;
    const uint8_t *end = (const uint8_t *)madt + madt->header.length;
    while (cursor + sizeof(struct madt_entry_header) <= end) {
        const struct madt_entry_header *entry = (const struct madt_entry_header *)cursor;
        if (entry->length < sizeof(*entry) || cursor + entry->length > end) return false;

        if (entry->type == MADT_ENTRY_IOAPIC &&
            entry->length >= sizeof(struct madt_ioapic_entry) &&
            unit_count < IOAPIC_MAX_UNITS) {
            const struct madt_ioapic_entry *firmware =
                (const struct madt_ioapic_entry *)entry;
            volatile uint32_t *mapping =
                (volatile uint32_t *)mmio_map(firmware->address, 4096u);
            if (mapping == 0) return false;

            struct ioapic_unit *unit = &units[unit_count++];
            unit->mmio = mapping;
            unit->gsi_base = firmware->gsi_base;
            unit->firmware_id = firmware->ioapic_id;
            const uint32_t version = ioapic_read(unit, IOAPIC_REG_VERSION);
            unit->redirection_count = ((version >> 16) & 0xffu) + 1u;
            (void)ioapic_read(unit, IOAPIC_REG_ID);
        } else if (entry->type == MADT_ENTRY_INTERRUPT_OVERRIDE &&
                   entry->length >= sizeof(struct madt_interrupt_override)) {
            const struct madt_interrupt_override *override =
                (const struct madt_interrupt_override *)entry;
            if (override->bus == 0u && override->source_irq < LEGACY_IRQ_COUNT) {
                struct legacy_route *route = &legacy_routes[override->source_irq];
                route->gsi = override->gsi;

                const uint16_t polarity = override->flags & 0x3u;
                const uint16_t trigger = (override->flags >> 2) & 0x3u;
                /* ISA conforming defaults are active-high, edge-triggered. */
                route->active_low = polarity == 3u;
                route->level_triggered = trigger == 3u;
            }
        }

        cursor += entry->length;
    }

    if (unit_count == 0 || !apic_enable_native()) {
        reset_state();
        return false;
    }

    /* Keep the legacy 8259 physically quiet while the IOAPIC owns the same
     * external lines. Existing handlers can retain their IRQ0..15 numbering. */
    outb(0x21u, 0xffu);
    outb(0xa1u, 0xffu);

    active = true;
    for (uint8_t irq = 0; irq < LEGACY_IRQ_COUNT; ++irq)
        (void)program_legacy_irq(irq, true);
    return true;
}

bool ioapic_is_active(void) {
    return active;
}

void ioapic_disable(void) {
    if (active) {
        for (uint8_t irq = 0; irq < LEGACY_IRQ_COUNT; ++irq)
            (void)program_legacy_irq(irq, true);
    }
    reset_state();
}

bool ioapic_mask_legacy_irq(uint8_t irq) {
    return program_legacy_irq(irq, true);
}

bool ioapic_unmask_legacy_irq(uint8_t irq) {
    return program_legacy_irq(irq, false);
}

uint32_t ioapic_gsi_for_legacy_irq(uint8_t irq) {
    if (irq >= LEGACY_IRQ_COUNT) return UINT32_MAX;
    return legacy_routes[irq].gsi;
}
