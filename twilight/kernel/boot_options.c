#include <stdbool.h>
#include <stddef.h>

#include <twilight/boot_options.h>

/*
 * entry.c owns Twilight's single LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID request.
 * Limine rejects duplicate requests with the same ID, so boot policy consumes
 * the command line passed in by entry.c instead of declaring another request.
 *
 * The normal Plasma image has no shell flag and therefore boots graphically.
 * Adding `nox.shell=1` (or `boot=shell`) requests the interactive shell.
 */
static bool boot_shell_requested;

static bool token_equal(const char *start, size_t length, const char *expected) {
    if (start == 0 || expected == 0) return false;
    size_t i = 0;
    while (i < length && expected[i] != '\0') {
        if (start[i] != expected[i]) return false;
        ++i;
    }
    return i == length && expected[i] == '\0';
}

void twilight_boot_options_init(const char *cmdline) {
    boot_shell_requested = false;
    if (cmdline == 0) return;

    const char *cursor = cmdline;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            ++cursor;
        if (*cursor == '\0') break;

        const char *start = cursor;
        while (*cursor != '\0' && *cursor != ' ' && *cursor != '\t' &&
               *cursor != '\r' && *cursor != '\n')
            ++cursor;
        const size_t length = (size_t)(cursor - start);

        if (token_equal(start, length, "nox.shell=1") ||
            token_equal(start, length, "boot=shell")) {
            boot_shell_requested = true;
            return;
        }
    }
}

bool twilight_boot_shell_requested(void) {
    return boot_shell_requested;
}
