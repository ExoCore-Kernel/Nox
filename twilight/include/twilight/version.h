#pragma once

#define TWILIGHT_VERSION "1.0.0"
#define TWILIGHT_RELEASE "RELEASE_x86_64_UNIVERSAL"
#define TWILIGHT_ARCH "x86_64"

extern const char twilight_build_date[];
extern const char twilight_build_user[];
extern const char twilight_build_id[];

const char *twilight_os_name_from_cmdline(const char *cmdline);
void twilight_print_version_banner(const char *os_name);
