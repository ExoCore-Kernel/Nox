#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool mmio_init(void);
bool mmio_is_initialized(void);
void *mmio_map(uint64_t physical_address, size_t length);
bool mmio_unmap(void *virtual_address);
