#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Discover IOAPICs and ISA interrupt-source overrides from ACPI MADT, enable
 * native Local APIC delivery, and program legacy IRQ0..15 routes masked. */
bool ioapic_init(void);
bool ioapic_is_active(void);
void ioapic_disable(void);

bool ioapic_mask_legacy_irq(uint8_t irq);
bool ioapic_unmask_legacy_irq(uint8_t irq);
uint32_t ioapic_gsi_for_legacy_irq(uint8_t irq);
