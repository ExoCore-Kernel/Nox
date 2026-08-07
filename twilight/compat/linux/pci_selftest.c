#include <stdbool.h>
#include <stddef.h>

#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/linux_compat.h>

static size_t probe_count;
static size_t remove_count;

static int selftest_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    (void)id;

    u16 vendor = 0;
    u16 device = 0;
    if (pci_read_config_word(pdev, 0x00, &vendor) != 0) return -1;
    if (pci_read_config_word(pdev, 0x02, &device) != 0) return -1;
    if (vendor != pdev->vendor || device != pdev->device) return -1;

    for (int bar = 0; bar < 6; ++bar) {
        const resource_size_t length = pci_resource_len(pdev, bar);
        const resource_size_t start = pci_resource_start(pdev, bar);
        const unsigned long flags = pci_resource_flags(pdev, bar);
        if (length == 0) continue;
        if (start == 0 && (flags & IORESOURCE_IO) != 0) {
            /* Unassigned I/O BARs are valid during very early firmware bring-up. */
            continue;
        }
        if ((flags & (IORESOURCE_IO | IORESOURCE_MEM)) == 0) return -1;
    }

    pci_set_drvdata(pdev, pdev);
    ++probe_count;
    return 0;
}

static void selftest_remove(struct pci_dev *pdev) {
    if (pci_get_drvdata(pdev) == pdev) ++remove_count;
    pci_set_drvdata(pdev, 0);
}

static const struct pci_device_id selftest_ids[] = {
    { PCI_DEVICE(PCI_ANY_ID, PCI_ANY_ID) },
    { 0 }
};

static struct pci_driver selftest_driver = {
    .name = "twilight-pci-compat-selftest",
    .id_table = selftest_ids,
    .probe = selftest_probe,
    .remove = selftest_remove,
};

bool linux_pci_compat_self_test(void) {
    const size_t count = linux_pci_device_count();
    if (count == 0) return false;

    probe_count = 0;
    remove_count = 0;

    if (pci_register_driver(&selftest_driver) != 0) return false;
    const bool probes_ok = probe_count == count;

    pci_unregister_driver(&selftest_driver);
    const bool removes_ok = remove_count == probe_count;

    if (probes_ok && removes_ok) {
        pr_info("PCI compat self-test: %zu device(s) matched through Linux pci_driver API", count);
    }
    return probes_ok && removes_ok;
}
