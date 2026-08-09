#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>

/*
 * Keep the graphical boot policy independent from entry.c's branding parser.
 * Limine fills request objects found in .limine_requests before kmain runs, so
 * the Linux userspace bring-up code can query the executable command line later
 * without coupling the generated ABI shim to entry.c internals.
 *
 * The normal Plasma image has no shell flag and therefore boots graphically.
 * Adding `nox.shell=1` to the Limine cmdline requests the interactive shell.
 */
__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request boot_options_cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

static bool token_equal(const char *start, size_t length, const char *expected) {
    if (start == 0 || expected == 0) return false;
    size_t i = 0;
    while (i < length && expected[i] != '\0') {
        if (start[i] != expected[i]) return false;
        ++i;
    }
    return i == length && expected[i] == '\0';
}

bool twilight_boot_shell_requested(void) {
    if (boot_options_cmdline_request.response == 0 ||
        boot_options_cmdline_request.response->cmdline == 0)
        return false;

    const char *cursor = boot_options_cmdline_request.response->cmdline;
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
            token_equal(start, length, "boot=shell"))
            return true;
    }

    return false;
}
