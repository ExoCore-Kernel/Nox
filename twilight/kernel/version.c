#include <stddef.h>

#include <twilight/log.h>
#include <twilight/version.h>

static char os_name_buffer[64] = "Unknown";

static void append(char *out, size_t cap, size_t *i, const char *text) {
    if (text == 0) return;
    while (*text != '\0' && *i + 1u < cap) {
        out[(*i)++] = *text++;
    }
    out[*i] = '\0';
}

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
    char line1[160] = {0};
    char line2[384] = {0};
    size_t i = 0;

    append(line1, sizeof(line1), &i, "Twilight ");
    append(line1, sizeof(line1), &i, os_name ? os_name : "Unknown");
    append(line1, sizeof(line1), &i, " ");
    append(line1, sizeof(line1), &i, TWILIGHT_VERSION);
    klog(line1);

    i = 0;
    append(line2, sizeof(line2), &i, "Twilight Kernel Version ");
    append(line2, sizeof(line2), &i, TWILIGHT_VERSION);
    append(line2, sizeof(line2), &i, ": ");
    append(line2, sizeof(line2), &i, twilight_build_date);
    append(line2, sizeof(line2), &i, "; ");
    append(line2, sizeof(line2), &i, twilight_build_user);
    append(line2, sizeof(line2), &i, ":");
    append(line2, sizeof(line2), &i, twilight_build_id);
    append(line2, sizeof(line2), &i, "/");
    append(line2, sizeof(line2), &i, TWILIGHT_RELEASE);
    append(line2, sizeof(line2), &i, " ");
    append(line2, sizeof(line2), &i, TWILIGHT_ARCH);
    klog(line2);
}
