#pragma once

#include <stdbool.h>

bool linux_compat_self_test(void);
bool linux_pci_compat_self_test(void);
bool linux_driver_runtime_init(void);
bool linux_driver_runtime_is_initialized(void);
bool linux_driver_runtime_self_test(void);
void linux_driver_runtime_poll(void);
