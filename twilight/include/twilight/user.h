#pragma once

#include <stdbool.h>
#include <stdint.h>

bool user_mode_is_available(void);
bool user_mode_self_test(void);

/* Entry point used by the DPL3 int 0x80 assembly stub. */
int user_syscall_dispatch(uint64_t syscall_number);
