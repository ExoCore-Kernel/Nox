#pragma once

#include <stdbool.h>

/* Parse the single Limine executable command line owned by entry.c and cache
 * Nox boot-policy flags for later userspace initcalls. */
void twilight_boot_options_init(const char *cmdline);

/* Normal Plasma images return false. `nox.shell=1` or `boot=shell` returns true. */
bool twilight_boot_shell_requested(void);
