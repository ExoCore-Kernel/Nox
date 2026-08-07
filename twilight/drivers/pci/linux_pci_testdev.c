/*
 * Twilight Linux-source compatibility proof driver for QEMU's pci-testdev.
 * Deliberately written against linux/* headers rather than Twilight-native APIs.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define QEMU_TESTDEV_VENDOR_ID 0x1b36u
#define QEMU_TESTDEV_DEVICE_ID 0x0005u

struct testdev_state {
    void __iomem *registers;
    u8 width;
    u32 test_offset;
    u8 test_data;
};

static int twilight_testdev_probe(struct pci_dev *pdev,
                                  const struct pci_device_id *id) {
    (void)id;

    int result = pci_enable_device(pdev);
    if (result != 0) return result;

    result = pci_request_regions(pdev, "twilight-linux-pci-testdev");
    if (result != 0) {
        pci_disable_device(pdev);
        return result;
    }

    struct testdev_state *state =
        (struct testdev_state *)kzalloc(sizeof(*state), GFP_KERNEL);
    if (state == 0) {
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -ENOMEM;
    }

    /* QEMU pci-testdev BAR0 is its MMIO test region. */
    if ((pci_resource_flags(pdev, 0) & IORESOURCE_MEM) == 0 ||
        pci_resource_len(pdev, 0) < 16u) {
        kfree(state);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -ENODEV;
    }

    state->registers = pci_iomap(pdev, 0, 0);
    if (state->registers == 0) {
        kfree(state);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -ENODEV;
    }

    /* Select QEMU's first MMIO test and read back its header. This proves
     * Linux-style PCI matching, BAR mapping, MMIO write and MMIO reads. */
    volatile u8 __iomem *regs = (volatile u8 __iomem *)state->registers;
    writeb(0u, (volatile void __iomem *)(regs + 0u));
    mb();
    state->width = readb((const volatile void __iomem *)(regs + 1u));
    state->test_offset = readl((const volatile void __iomem *)(regs + 4u));
    state->test_data = readb((const volatile void __iomem *)(regs + 8u));

    if (state->width == 0u || state->test_offset == 0u) {
        pci_iounmap(pdev, state->registers);
        kfree(state);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -EIO;
    }

    pci_set_drvdata(pdev, state);
    pr_info("pci-testdev: Linux-style driver bound to %s; BAR0=%p len=%llu width=%u test-offset=0x%x data=0x%x",
            dev_name(&pdev->dev),
            state->registers,
            (unsigned long long)pci_resource_len(pdev, 0),
            (unsigned)state->width,
            state->test_offset,
            (unsigned)state->test_data);
    return 0;
}

static void twilight_testdev_remove(struct pci_dev *pdev) {
    struct testdev_state *state =
        (struct testdev_state *)pci_get_drvdata(pdev);
    if (state != 0) {
        if (state->registers != 0) pci_iounmap(pdev, state->registers);
        kfree(state);
    }

    pci_set_drvdata(pdev, 0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pr_info("pci-testdev: driver removed from %s", dev_name(&pdev->dev));
}

static const struct pci_device_id twilight_testdev_ids[] = {
    { PCI_DEVICE(QEMU_TESTDEV_VENDOR_ID, QEMU_TESTDEV_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, twilight_testdev_ids);

static struct pci_driver twilight_testdev_driver = {
    .name = "twilight-linux-pci-testdev",
    .id_table = twilight_testdev_ids,
    .probe = twilight_testdev_probe,
    .remove = twilight_testdev_remove,
};

module_pci_driver(twilight_testdev_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Twilight compatibility bring-up");
MODULE_DESCRIPTION("QEMU pci-testdev proof driver using Twilight Linux PCI compatibility");
