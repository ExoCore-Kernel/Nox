#pragma once

#define DMI_SYS_VENDOR  0
#define DMI_PRODUCT_NAME 1

struct dmi_strmatch {
    unsigned char slot;
    const char *substr;
};

struct dmi_system_id {
    const char *ident;
    struct dmi_strmatch matches[4];
    void *driver_data;
};

#define DMI_MATCH(slot, text) { .slot = (slot), .substr = (text) }

/* Twilight does not yet expose SMBIOS/DMI strings to Linux compatibility.
 * Returning no match is conservative and only disables board-specific quirks. */
static inline int dmi_check_system(const struct dmi_system_id *list) {
    (void)list;
    return 0;
}
