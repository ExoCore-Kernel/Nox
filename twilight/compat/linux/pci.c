#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <twilight/pci.h>

#define LINUX_PCI_MAX_DRIVERS 64u

static struct pci_dev linux_devices[TWILIGHT_PCI_MAX_DEVICES];
static struct pci_bus linux_buses[256];
static char linux_device_names[TWILIGHT_PCI_MAX_DEVICES][16];
static struct pci_driver *registered_drivers[LINUX_PCI_MAX_DRIVERS];
static size_t registered_driver_count;
static bool wrappers_ready;

extern struct pci_driver *__twilight_pci_drivers_start[];
extern struct pci_driver *__twilight_pci_drivers_end[];

static void write_decimal(char *buffer, size_t *position, unsigned value) {
    if (value >= 100u) buffer[(*position)++] = (char)('0' + (value / 100u) % 10u);
    if (value >= 10u) buffer[(*position)++] = (char)('0' + (value / 10u) % 10u);
    buffer[(*position)++] = (char)('0' + value % 10u);
}

static char hex_digit(unsigned value) {
    return "0123456789abcdef"[value & 0xfu];
}

static void build_device_name(char out[16], const struct twilight_pci_device *native) {
    size_t position = 0;
    write_decimal(out, &position, native->bus);
    out[position++] = ':';
    out[position++] = hex_digit(native->slot >> 4);
    out[position++] = hex_digit(native->slot);
    out[position++] = '.';
    out[position++] = hex_digit(native->function);
    out[position] = '\0';
}

static bool ensure_wrappers(void) {
    if (wrappers_ready) return true;
    if (!pci_is_initialized()) return false;

    const size_t count = pci_device_count();
    if (count > TWILIGHT_PCI_MAX_DEVICES) return false;

    for (unsigned bus = 0; bus < 256u; ++bus) {
        linux_buses[bus].number = (u8)bus;
    }

    for (size_t i = 0; i < count; ++i) {
        struct twilight_pci_device *native = pci_device_at(i);
        if (native == 0) return false;

        struct pci_dev *pdev = &linux_devices[i];
        *pdev = (struct pci_dev){0};
        build_device_name(linux_device_names[i], native);

        pdev->dev.init_name = linux_device_names[i];
        pdev->dev.driver_data = native->driver_data;
        pdev->bus = &linux_buses[native->bus];
        pdev->devfn = PCI_DEVFN(native->slot, native->function);
        pdev->vendor = native->vendor_id;
        pdev->device = native->device_id;
        pdev->subsystem_vendor = native->subsystem_vendor_id;
        pdev->subsystem_device = native->subsystem_device_id;
        pdev->class = ((u32)native->class_code << 16) |
                      ((u32)native->subclass << 8) |
                      (u32)native->prog_if;
        pdev->revision = native->revision;
        pdev->irq = native->irq_line;
        pdev->twilight = native;
    }

    wrappers_ready = true;
    return true;
}

size_t linux_pci_device_count(void) {
    return ensure_wrappers() ? pci_device_count() : 0;
}

struct pci_dev *linux_pci_device_at(size_t index) {
    if (!ensure_wrappers() || index >= pci_device_count()) return 0;
    return &linux_devices[index];
}

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *pdev) {
    if (ids == 0 || pdev == 0) return 0;

    for (const struct pci_device_id *id = ids;; ++id) {
        const bool terminator = id->vendor == 0 && id->device == 0 &&
                                id->subvendor == 0 && id->subdevice == 0 &&
                                id->class == 0 && id->class_mask == 0 &&
                                id->driver_data == 0;
        if (terminator) return 0;

        if (id->vendor != PCI_ANY_ID && id->vendor != pdev->vendor) continue;
        if (id->device != PCI_ANY_ID && id->device != pdev->device) continue;
        if (id->subvendor != PCI_ANY_ID && id->subvendor != pdev->subsystem_vendor) continue;
        if (id->subdevice != PCI_ANY_ID && id->subdevice != pdev->subsystem_device) continue;
        if (((pdev->class ^ id->class) & id->class_mask) != 0) continue;
        return id;
    }
}

int pci_register_driver(struct pci_driver *driver) {
    if (driver == 0 || driver->id_table == 0 || driver->probe == 0) return -EINVAL;
    if (!ensure_wrappers()) return -ENODEV;

    for (size_t i = 0; i < registered_driver_count; ++i) {
        if (registered_drivers[i] == driver) return 0;
    }
    if (registered_driver_count >= LINUX_PCI_MAX_DRIVERS) return -ENOSPC;

    registered_drivers[registered_driver_count++] = driver;

    for (size_t i = 0; i < pci_device_count(); ++i) {
        struct pci_dev *pdev = &linux_devices[i];
        struct twilight_pci_device *native = pdev->twilight;
        if (native == 0 || native->bound_driver != 0) continue;

        const struct pci_device_id *id = pci_match_id(driver->id_table, pdev);
        if (id == 0) continue;

        const int result = driver->probe(pdev, id);
        if (result == 0) {
            native->bound_driver = driver;
            native->driver_data = pdev->dev.driver_data;
        }
    }

    return 0;
}

void pci_unregister_driver(struct pci_driver *driver) {
    if (driver == 0 || !ensure_wrappers()) return;

    for (size_t i = 0; i < pci_device_count(); ++i) {
        struct pci_dev *pdev = &linux_devices[i];
        struct twilight_pci_device *native = pdev->twilight;
        if (native == 0 || native->bound_driver != driver) continue;

        if (driver->remove != 0) driver->remove(pdev);
        native->driver_data = pdev->dev.driver_data;
        native->bound_driver = 0;
    }

    for (size_t i = 0; i < registered_driver_count; ++i) {
        if (registered_drivers[i] != driver) continue;
        for (size_t j = i + 1; j < registered_driver_count; ++j) {
            registered_drivers[j - 1] = registered_drivers[j];
        }
        --registered_driver_count;
        registered_drivers[registered_driver_count] = 0;
        break;
    }
}

int linux_pci_register_builtin_drivers(void) {
    if (!ensure_wrappers()) return -ENODEV;

    int first_error = 0;
    for (struct pci_driver **entry = __twilight_pci_drivers_start;
         entry < __twilight_pci_drivers_end;
         ++entry) {
        if (*entry == 0) continue;
        const int result = pci_register_driver(*entry);
        if (result != 0 && first_error == 0) first_error = result;
    }
    return first_error;
}

int pci_enable_device(struct pci_dev *pdev) {
    if (pdev == 0 || pdev->twilight == 0) return -ENODEV;
    return pci_enable_device_native(pdev->twilight) ? 0 : -EIO;
}

void pci_disable_device(struct pci_dev *pdev) {
    if (pdev == 0 || pdev->twilight == 0) return;
    u16 command = pci_config_read16(pdev->twilight, PCI_COMMAND);
    command &= (u16)~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY);
    pci_config_write16(pdev->twilight, PCI_COMMAND, command);
}

void pci_set_master(struct pci_dev *pdev) {
    if (pdev != 0 && pdev->twilight != 0) pci_set_bus_master_native(pdev->twilight, true);
}

void pci_clear_master(struct pci_dev *pdev) {
    if (pdev != 0 && pdev->twilight != 0) pci_set_bus_master_native(pdev->twilight, false);
}

resource_size_t pci_resource_start(struct pci_dev *pdev, int bar) {
    if (pdev == 0 || pdev->twilight == 0 || bar < 0 || bar >= (int)TWILIGHT_PCI_MAX_BARS) return 0;
    return (resource_size_t)pdev->twilight->bars[bar].address;
}

resource_size_t pci_resource_len(struct pci_dev *pdev, int bar) {
    if (pdev == 0 || pdev->twilight == 0 || bar < 0 || bar >= (int)TWILIGHT_PCI_MAX_BARS) return 0;
    return (resource_size_t)pdev->twilight->bars[bar].size;
}

unsigned long pci_resource_flags(struct pci_dev *pdev, int bar) {
    if (pdev == 0 || pdev->twilight == 0 || bar < 0 || bar >= (int)TWILIGHT_PCI_MAX_BARS) return 0;
    const struct twilight_pci_bar *native_bar = &pdev->twilight->bars[bar];
    if (native_bar->size == 0) return 0;

    unsigned long flags = (native_bar->flags & TWILIGHT_PCI_BAR_IO) != 0
        ? (unsigned long)IORESOURCE_IO
        : (unsigned long)IORESOURCE_MEM;
    if ((native_bar->flags & TWILIGHT_PCI_BAR_MEM64) != 0) flags |= (unsigned long)IORESOURCE_MEM_64;
    if ((native_bar->flags & TWILIGHT_PCI_BAR_PREFETCH) != 0) flags |= (unsigned long)IORESOURCE_PREFETCH;
    return flags;
}

void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen) {
    const unsigned long flags = pci_resource_flags(pdev, bar);
    if ((flags & IORESOURCE_MEM) == 0) return 0;

    const resource_size_t start = pci_resource_start(pdev, bar);
    resource_size_t length = pci_resource_len(pdev, bar);
    if (start == 0 || length == 0) return 0;
    if (maxlen != 0 && (resource_size_t)maxlen < length) length = (resource_size_t)maxlen;
    return ioremap((phys_addr_t)start, (size_t)length);
}

void pci_iounmap(struct pci_dev *pdev, void __iomem *address) {
    (void)pdev;
    if (address != 0) iounmap(address);
}

int pci_request_regions(struct pci_dev *pdev, const char *name) {
    (void)name;
    return pdev != 0 ? 0 : -ENODEV;
}

void pci_release_regions(struct pci_dev *pdev) {
    (void)pdev;
}

static bool valid_config_request(const struct pci_dev *pdev, int where, int width) {
    return pdev != 0 && pdev->twilight != 0 && where >= 0 &&
           where + width <= 256 && (where & (width - 1)) == 0;
}

int pci_read_config_byte(const struct pci_dev *pdev, int where, u8 *value) {
    if (value == 0 || !valid_config_request(pdev, where, 1)) return -EINVAL;
    *value = pci_config_read8(pdev->twilight, (u16)where);
    return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_word(const struct pci_dev *pdev, int where, u16 *value) {
    if (value == 0 || !valid_config_request(pdev, where, 2)) return -EINVAL;
    *value = pci_config_read16(pdev->twilight, (u16)where);
    return PCIBIOS_SUCCESSFUL;
}

int pci_read_config_dword(const struct pci_dev *pdev, int where, u32 *value) {
    if (value == 0 || !valid_config_request(pdev, where, 4)) return -EINVAL;
    *value = pci_config_read32(pdev->twilight, (u16)where);
    return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_byte(const struct pci_dev *pdev, int where, u8 value) {
    if (!valid_config_request(pdev, where, 1)) return -EINVAL;
    pci_config_write8(pdev->twilight, (u16)where, value);
    return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_word(const struct pci_dev *pdev, int where, u16 value) {
    if (!valid_config_request(pdev, where, 2)) return -EINVAL;
    pci_config_write16(pdev->twilight, (u16)where, value);
    return PCIBIOS_SUCCESSFUL;
}

int pci_write_config_dword(const struct pci_dev *pdev, int where, u32 value) {
    if (!valid_config_request(pdev, where, 4)) return -EINVAL;
    pci_config_write32(pdev->twilight, (u16)where, value);
    return PCIBIOS_SUCCESSFUL;
}

int pci_find_capability(struct pci_dev *pdev, int capability) {
    if (pdev == 0) return 0;

    u16 status = 0;
    if (pci_read_config_word(pdev, PCI_STATUS, &status) != 0 ||
        (status & PCI_STATUS_CAP_LIST) == 0) {
        return 0;
    }

    u8 pointer = 0;
    if (pci_read_config_byte(pdev, PCI_CAPABILITY_LIST, &pointer) != 0) return 0;
    pointer &= (u8)~3u;

    for (unsigned hops = 0; hops < 48u && pointer >= 0x40u; ++hops) {
        u8 id = 0;
        u8 next = 0;
        if (pci_read_config_byte(pdev, pointer, &id) != 0) return 0;
        if ((int)id == capability) return pointer;
        if (pci_read_config_byte(pdev, pointer + 1u, &next) != 0) return 0;
        pointer = (u8)(next & (u8)~3u);
    }

    return 0;
}

struct pci_dev *pci_get_device(unsigned int vendor,
                               unsigned int device,
                               struct pci_dev *from) {
    if (!ensure_wrappers()) return 0;

    size_t start = 0;
    if (from != 0) {
        if (from < &linux_devices[0] || from >= &linux_devices[pci_device_count()]) return 0;
        start = (size_t)(from - &linux_devices[0]) + 1u;
    }

    for (size_t i = start; i < pci_device_count(); ++i) {
        struct pci_dev *pdev = &linux_devices[i];
        if (vendor != PCI_ANY_ID && pdev->vendor != vendor) continue;
        if (device != PCI_ANY_ID && pdev->device != device) continue;
        return pdev;
    }
    return 0;
}

void pci_dev_put(struct pci_dev *pdev) {
    (void)pdev;
}
