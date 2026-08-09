#include <stdbool.h>
#include <stddef.h>

#include <twilight/log.h>
#include <twilight/serial.h>
#include <twilight/version.h>

static char os_name_buffer[64] = "Unknown";
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

static void cache_boot_flags(const char *cmdline) {
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

bool twilight_boot_shell_requested_from_cmdline(void) {
    return boot_shell_requested;
}

const char *twilight_os_name_from_cmdline(const char *cmdline) {
    static const char key[] = "os_name=";
    if (cmdline == 0) return os_name_buffer;

    /* entry.c already owns the one Limine executable-cmdline request. Cache
     * boot-policy flags while parsing that same response so no second request
     * object is needed anywhere in the kernel. */
    cache_boot_flags(cmdline);

    for (size_t p = 0; cmdline[p] != '\0'; ++p) {
        size_t k = 0;
        while (key[k] != '\0' && cmdline[p + k] == key[k]) ++k;
        if (key[k] != '\0') continue;

        size_t o = 0;
        size_t s = p + k;
        while (cmdline[s] != '\0' && cmdline[s] != ' ' && o + 1u < sizeof(os_name_buffer)) {
            os_name_buffer[o++] = cmdline[s++];
        }
        os_name_buffer[o] = '\0';
        if (o == 0) {
            os_name_buffer[0] = 'U'; os_name_buffer[1] = 'n'; os_name_buffer[2] = 'k';
            os_name_buffer[3] = 'n'; os_name_buffer[4] = 'o'; os_name_buffer[5] = 'w';
            os_name_buffer[6] = 'n'; os_name_buffer[7] = '\0';
        }
        return os_name_buffer;
    }
    return os_name_buffer;
}

void twilight_print_version_banner(const char *os_name) {
    const char *line1[] = {
        os_name ? os_name : "Unknown",
        " ",
        TWILIGHT_VERSION,
    };
    klog_parts(line1, sizeof(line1) / sizeof(line1[0]));

    /* Keep framebuffer boot unblocked while the long metadata line is debugged. */
    klog("Twilight Kernel Version " TWILIGHT_VERSION);

    serial_write("[serial] full build metadata: Twilight Kernel Version ");
    serial_write(TWILIGHT_VERSION);
    serial_write(": ");
    serial_write(twilight_build_date);
    serial_write("; ");
    serial_write(twilight_build_user);
    serial_write(":");
    serial_write(twilight_build_id);
    serial_write("/");
    serial_write(TWILIGHT_RELEASE);
    serial_write(" ");
    serial_write(TWILIGHT_ARCH);
    serial_write("\n");
}
