#pragma once

#include <stdint.h>

__attribute__((noreturn)) void kernel_panic(const char *reason);
__attribute__((noreturn)) void kernel_panic_exception(uint64_t vector,
                                                      uint64_t error_code,
                                                      uint64_t rip);
