#pragma once

#include <stdbool.h>
#include <stdint.h>

bool apic_disable_for_legacy_pic(void);
bool apic_enable_virtual_wire_for_legacy_pic(void);

/* Native xAPIC mode used with IOAPIC/MSI delivery. */
bool apic_enable_native(void);
bool apic_native_enabled(void);
uint8_t apic_id(void);

/* Sends a Local-APIC EOI whenever Twilight is using either native xAPIC or
 * legacy virtual-wire mode. Safe to call when no APIC is active. */
void apic_eoi_if_needed(void);
