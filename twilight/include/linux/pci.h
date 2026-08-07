#pragma once

#include <stddef.h>
#include <stdint.h>

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/types.h>

#define PCI_ANY_ID (~0u)
#define PCI_VENDOR_ID_REDHAT  0x1b36u
#define PCI_VENDOR_ID_REALTEK 0x10ecu
#define PCI_DEVICE_ID_REALTEK_8139 0x8139u
#define PCI_VENDOR_ID_ATHEROS 0x168cu

#define PCI_COMMAND          0x04
#define PCI_COMMAND_IO       0x0001
#define PCI_COMMAND_MEMORY   0x0002
#define PCI_COMMAND_MASTER   0x0004
#define PCI_STATUS           0x06
#define PCI_STATUS_CAP_LIST  0x0010
#define PCI_REVISION_ID      0x08
#define PCI_CLASS_PROG       0x09
#define PCI_CLASS_DEVICE     0x0a
#define PCI_BASE_ADDRESS_0   0x10
#define PCI_CAPABILITY_LIST  0x34
#define PCI_INTERRUPT_LINE   0x3c
#define PCI_INTERRUPT_PIN    0x3d

#define PCI_CAP_ID_PM        0x01
#define PCI_CAP_ID_MSI       0x05
#define PCI_CAP_ID_EXP       0x10
#define PCI_CAP_ID_MSIX      0x11

#define IORESOURCE_IO        0x00000100ull
#define IORESOURCE_MEM       0x00000200ull
#define IORESOURCE_PREFETCH  0x00002000ull
#define IORESOURCE_MEM_64    0x00100000ull

#define PCIBIOS_SUCCESSFUL 0

struct twilight_pci_device;

struct pci_bus { u8 number; };

struct pci_device_id {
    u32 vendor;
    u32 device;
    u32 subvendor;
    u32 subdevice;
    u32 class;
    u32 class_mask;
    unsigned long driver_data;
};

struct pci_dev {
    struct device dev;
    struct pci_bus *bus;
    unsigned int devfn;
    u16 vendor;
    u16 device;
    u16 subsystem_vendor;
    u16 subsystem_device;
    u32 class;
    u8 revision;
    unsigned int irq;
    struct twilight_pci_device *twilight;
};

struct pci_driver {
    const char *name;
    const struct pci_device_id *id_table;
    int (*probe)(struct pci_dev *pdev, const struct pci_device_id *id);
    void (*remove)(struct pci_dev *pdev);
    const void *dev_groups;
};

#define PCI_SLOT(devfn) (((devfn) >> 3) & 0x1f)
#define PCI_FUNC(devfn) ((devfn) & 0x07)
#define PCI_DEVFN(slot, func) ((((slot) & 0x1f) << 3) | ((func) & 0x07))

#define PCI_DEVICE(vend, dev) \
    .vendor = (vend), .device = (dev), \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID

#define PCI_DEVICE_SUB(vend, dev, subvend, subdev) \
    .vendor = (vend), .device = (dev), \
    .subvendor = (subvend), .subdevice = (subdev)

#define PCI_DEVICE_CLASS(dev_class, dev_class_mask) \
    .vendor = PCI_ANY_ID, .device = PCI_ANY_ID, \
    .subvendor = PCI_ANY_ID, .subdevice = PCI_ANY_ID, \
    .class = (dev_class), .class_mask = (dev_class_mask)

int pci_register_driver(struct pci_driver *driver);
void pci_unregister_driver(struct pci_driver *driver);
int linux_pci_register_builtin_drivers(void);
size_t linux_pci_device_count(void);
struct pci_dev *linux_pci_device_at(size_t index);

const struct pci_device_id *pci_match_id(const struct pci_device_id *ids,
                                         struct pci_dev *pdev);

int pci_enable_device(struct pci_dev *pdev);
void pci_disable_device(struct pci_dev *pdev);
int pcim_enable_device(struct pci_dev *pdev);
void pci_set_master(struct pci_dev *pdev);
void pci_clear_master(struct pci_dev *pdev);

resource_size_t pci_resource_start(struct pci_dev *pdev, int bar);
resource_size_t pci_resource_len(struct pci_dev *pdev, int bar);
unsigned long pci_resource_flags(struct pci_dev *pdev, int bar);
static inline resource_size_t pci_resource_end(struct pci_dev *pdev, int bar) {
    const resource_size_t start = pci_resource_start(pdev, bar);
    const resource_size_t length = pci_resource_len(pdev, bar);
    return length == 0 ? start : start + length - 1u;
}

void __iomem *pci_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);
void pci_iounmap(struct pci_dev *pdev, void __iomem *address);
void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen);

int pci_request_regions(struct pci_dev *pdev, const char *name);
void pci_release_regions(struct pci_dev *pdev);

int pci_read_config_byte(const struct pci_dev *pdev, int where, u8 *value);
int pci_read_config_word(const struct pci_dev *pdev, int where, u16 *value);
int pci_read_config_dword(const struct pci_dev *pdev, int where, u32 *value);
int pci_write_config_byte(const struct pci_dev *pdev, int where, u8 value);
int pci_write_config_word(const struct pci_dev *pdev, int where, u16 value);
int pci_write_config_dword(const struct pci_dev *pdev, int where, u32 value);

int pci_find_capability(struct pci_dev *pdev, int capability);
struct pci_dev *pci_get_device(unsigned int vendor,
                               unsigned int device,
                               struct pci_dev *from);
void pci_dev_put(struct pci_dev *pdev);

const char *pci_name(const struct pci_dev *pdev);

static inline void pci_set_drvdata(struct pci_dev *pdev, void *data) {
    if (pdev != 0) dev_set_drvdata(&pdev->dev, data);
}

static inline void *pci_get_drvdata(struct pci_dev *pdev) {
    return pdev != 0 ? dev_get_drvdata(&pdev->dev) : 0;
}

static inline int pci_enable_msi(struct pci_dev *pdev) {
    (void)pdev;
    return -ENOSYS;
}
static inline void pci_disable_msi(struct pci_dev *pdev) { (void)pdev; }

#define module_pci_driver(__pci_driver) \
    static struct pci_driver * const \
    __twilight_builtin_pci_driver_##__pci_driver \
    __attribute__((used, section(".twilight_pci_drivers"))) = &(__pci_driver)
