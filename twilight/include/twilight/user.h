#pragma once

#include <stdbool.h>
#include <stdint.h>

bool user_mode_is_available(void);
bool user_mode_self_test(void);
bool linux_user_self_test(void);

/* Entry point used by the DPL3 int 0x80 assembly stub. */
int user_syscall_dispatch(uint64_t syscall_number);

/* Linux x86-64 syscall dispatcher used by the SYSCALL entry stub. */
int64_t linux_syscall_dispatch(uint64_t syscall_number,
                               uint64_t arg1,
                               uint64_t arg2,
                               uint64_t arg3,
                               uint64_t arg4,
                               uint64_t arg5,
                               uint64_t arg6);
