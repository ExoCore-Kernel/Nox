#include <stdbool.h>

#include <twilight/boot_options.h>
#include <twilight/version.h>

/*
 * entry.c owns Twilight's single LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID request.
 * twilight_os_name_from_cmdline() is already called from entry.c and caches the
 * Nox boot flags from that same command line. Keep this small boot-policy API as
 * the stable interface used by the generated Plasma userspace compatibility
 * unit, without declaring any additional Limine request objects.
 */
void twilight_boot_options_init(const char *cmdline) {
    /* Kept for a stable API. Parsing is performed by the existing version/cmdline
     * path in entry.c so there is exactly one owner of the Limine response. */
    (void)cmdline;
}

bool twilight_boot_shell_requested(void) {
    return twilight_boot_shell_requested_from_cmdline();
}
