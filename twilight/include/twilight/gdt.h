#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TWILIGHT_GDT_KERNEL_CODE 0x08u
#define TWILIGHT_GDT_KERNEL_DATA 0x10u
#define TWILIGHT_GDT_USER_DATA   0x1bu
#define TWILIGHT_GDT_USER_CODE   0x23u
#define TWILIGHT_GDT_TSS         0x28u

bool gdt_init(void);
bool gdt_is_initialized(void);
uint64_t gdt_kernel_stack(void);
