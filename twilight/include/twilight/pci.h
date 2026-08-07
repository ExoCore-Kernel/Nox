#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TWILIGHT_PCI_MAX_BARS 6u
#define TWILIGHT_PCI_MAX_DEVICES 512u

#define TWILIGHT_PCI_BAR_IO       (1u << 0)
#define TWILIGHT_PCI_BAR_MEM64    (1u << 1)
#define TWILIGHT_PCI_BAR_PREFETCH (1u << 2)

struct twilight_pci_bar {
    uint64_t address;
    uint64_t size;
    uint32_t flags;
};

struct twilight_pci_device {
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_device_id;

    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t header_type;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;

    uint8_t irq_line;
    uint8_t irq_pin;

    struct twilight_pci_bar bars[TWILIGHT_PCI_MAX_BARS];
    void *driver_data;
    void *bound_driver;
};

bool pci_init(void);
bool pci_is_initialized(void);
size_t pci_device_count(void);
struct twilight_pci_device *pci_device_at(size_t index);

uint8_t pci_config_read8(const struct twilight_pci_device *device, uint16_t offset);
uint16_t pci_config_read16(const struct twilight_pci_device *device, uint16_t offset);
uint32_t pci_config_read32(const struct twilight_pci_device *device, uint16_t offset);
void pci_config_write8(const struct twilight_pci_device *device, uint16_t offset, uint8_t value);
void pci_config_write16(const struct twilight_pci_device *device, uint16_t offset, uint16_t value);
void pci_config_write32(const struct twilight_pci_device *device, uint16_t offset, uint32_t value);

bool pci_enable_device_native(struct twilight_pci_device *device);
void pci_set_bus_master_native(struct twilight_pci_device *device, bool enabled);
