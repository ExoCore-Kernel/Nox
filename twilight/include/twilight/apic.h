#pragma once

#include <stdbool.h>

bool apic_disable_for_legacy_pic(void);
bool apic_enable_virtual_wire_for_legacy_pic(void);
void apic_eoi_if_needed(void);
