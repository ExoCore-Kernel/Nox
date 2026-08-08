#include <stddef.h>

#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/pci.h>

/*
 * Generic diagnostics for the driver-compatibility matrix.  This deliberately
 * knows nothing about QEMU device models or particular vendor/device IDs: it
 * reports whatever native PCI enumeration discovered and whether the ordinary
 * Linux PCI matching/probe path bound a driver to it.
 */
void linux_pci_report_bindings(void) {
    const size_t count = linux_pci_device_count();
    pr_info("PCI binding inventory: %zu enumerated device(s)", count);

    for (size_t i = 0; i < count; ++i) {
        struct pci_dev *pdev = linux_pci_device_at(i);
        if (pdev == 0 || pdev->twilight == 0) continue;

        const struct twilight_pci_device *native = pdev->twilight;
        const struct pci_driver *driver =
            (const struct pci_driver *)native->bound_driver;

        pr_info("PCI BIND %s id=%04x:%04x class=%06x irq=%u driver=%s",
                pci_name(pdev),
                (unsigned)pdev->vendor,
                (unsigned)pdev->device,
                (unsigned)pdev->class,
                pdev->irq,
                driver != 0 && driver->name != 0 ? driver->name : "NONE");
    }
}
