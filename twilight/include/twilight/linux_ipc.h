#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Early Linux IPC compatibility hook used by the generated Bash syscall shim.
 * Returns a Linux syscall result when *handled is true. When *handled is false,
 * the caller must continue through its normal syscall dispatcher. */
int64_t twilight_linux_ipc_syscall(uint64_t number,
                                  uint64_t a1, uint64_t a2, uint64_t a3,
                                  uint64_t a4, uint64_t a5, uint64_t a6,
                                  bool *handled);

/* Drop every IPC descriptor owned by the current early userspace process.
 * Final AF_UNIX endpoint references propagate EOF/POLLHUP to their peer. */
void twilight_linux_ipc_process_exit(void);
