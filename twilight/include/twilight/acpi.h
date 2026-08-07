#pragma once

#include <stdbool.h>
#include <stdint.h>

struct __attribute__((packed)) acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

bool acpi_init(const void *rsdp_address);
bool acpi_is_initialized(void);
const struct acpi_sdt_header *acpi_find_table(const char signature[4]);
