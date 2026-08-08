#pragma once

#include <stdbool.h>

struct cpu_security_status {
    bool write_protect;
    bool smep;
    bool smap;
    bool umip;
    bool fpu_sse;
};

bool cpu_security_init(void);
void cpu_security_get_status(struct cpu_security_status *out);

/* Use these only around carefully validated kernel accesses to user pages. */
void cpu_user_access_begin(void);
void cpu_user_access_end(void);
