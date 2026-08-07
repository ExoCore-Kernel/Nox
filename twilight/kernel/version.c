#include <stddef.h>

#include <twilight/log.h>
#include <twilight/serial.h>
#include <twilight/version.h>

static char os_name_buffer[64] = "Unknown";

const char *twilight_os_name_from_cmdline(const char *cmdline) {
    static const char key[] = "os_name=";
    if (cmdline == 0) return os_name_buffer;

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
        "Twilight ",
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
