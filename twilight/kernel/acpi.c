#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/acpi.h>
#include <twilight/pmm.h>

struct __attribute__((packed)) acpi_rsdp_v1 {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
};

struct __attribute__((packed)) acpi_rsdp_v2 {
    struct acpi_rsdp_v1 first;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

static const struct acpi_sdt_header *root_table;
static size_t root_entry_size;
static bool initialized;

static bool bytes_equal(const char *a, const char *b, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool checksum_ok(const void *pointer, size_t size) {
    const uint8_t *bytes = (const uint8_t *)pointer;
    uint8_t sum = 0;
    for (size_t i = 0; i < size; ++i) sum = (uint8_t)(sum + bytes[i]);
    return sum == 0;
}

static const void *physical_pointer(uint64_t physical) {
    return pmm_phys_to_virt(physical);
}

bool acpi_init(const void *rsdp_address) {
    initialized = false;
    root_table = 0;
    root_entry_size = 0;

    if (!pmm_is_initialized() || rsdp_address == 0) return false;

    const struct acpi_rsdp_v1 *rsdp1 = (const struct acpi_rsdp_v1 *)rsdp_address;
    if (!bytes_equal(rsdp1->signature, "RSD PTR ", 8)) return false;
    if (!checksum_ok(rsdp1, sizeof(*rsdp1))) return false;

    uint64_t root_physical = rsdp1->rsdt_address;
    const char *expected_signature = "RSDT";
    root_entry_size = sizeof(uint32_t);

    if (rsdp1->revision >= 2) {
        const struct acpi_rsdp_v2 *rsdp2 = (const struct acpi_rsdp_v2 *)rsdp_address;
        if (rsdp2->length < sizeof(*rsdp2)) return false;
        if (!checksum_ok(rsdp2, rsdp2->length)) return false;
        if (rsdp2->xsdt_address != 0) {
            root_physical = rsdp2->xsdt_address;
            expected_signature = "XSDT";
            root_entry_size = sizeof(uint64_t);
        }
    }

    if (root_physical == 0) return false;
    root_table = (const struct acpi_sdt_header *)physical_pointer(root_physical);
    if (root_table == 0 || root_table->length < sizeof(*root_table)) return false;
    if (!bytes_equal(root_table->signature, expected_signature, 4)) return false;
    if (!checksum_ok(root_table, root_table->length)) return false;

    const size_t payload = root_table->length - sizeof(*root_table);
    if ((payload % root_entry_size) != 0) return false;

    initialized = true;
    return true;
}

bool acpi_is_initialized(void) {
    return initialized;
}

const struct acpi_sdt_header *acpi_find_table(const char signature[4]) {
    if (!initialized || signature == 0 || root_table == 0) return 0;

    const uint8_t *entries = (const uint8_t *)root_table + sizeof(*root_table);
    const size_t count = (root_table->length - sizeof(*root_table)) / root_entry_size;

    for (size_t i = 0; i < count; ++i) {
        uint64_t physical = 0;
        if (root_entry_size == sizeof(uint64_t)) {
            const uint8_t *p = entries + i * sizeof(uint64_t);
            for (unsigned byte = 0; byte < 8; ++byte) physical |= (uint64_t)p[byte] << (byte * 8u);
        } else {
            const uint8_t *p = entries + i * sizeof(uint32_t);
            for (unsigned byte = 0; byte < 4; ++byte) physical |= (uint64_t)p[byte] << (byte * 8u);
        }

        if (physical == 0) continue;
        const struct acpi_sdt_header *table =
            (const struct acpi_sdt_header *)physical_pointer(physical);
        if (table == 0 || table->length < sizeof(*table)) continue;
        if (!bytes_equal(table->signature, signature, 4)) continue;
        if (!checksum_ok(table, table->length)) continue;
        return table;
    }

    return 0;
}
