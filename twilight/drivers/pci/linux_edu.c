/*
 * Twilight Linux-source compatibility proof driver for QEMU's EDU PCI device.
 * Deliberately written against linux/* headers rather than Twilight-native APIs.
 */
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>

#define QEMU_EDU_VENDOR_ID 0x1234u
#define QEMU_EDU_DEVICE_ID 0x11e8u

struct edu_state {
    void __iomem *registers;
    u32 identity;
};

static int twilight_edu_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    (void)id;

    int result = pci_enable_device(pdev);
    if (result != 0) return result;

    result = pci_request_regions(pdev, "twilight-linux-edu");
    if (result != 0) {
        pci_disable_device(pdev);
        return result;
    }

    struct edu_state *state = (struct edu_state *)kzalloc(sizeof(*state), GFP_KERNEL);
    if (state == 0) {
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -ENOMEM;
    }

    state->registers = pci_iomap(pdev, 0, 0);
    if (state->registers == 0) {
        kfree(state);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -ENODEV;
    }

    state->identity = readl(state->registers);
    if (state->identity == 0 || state->identity == 0xffffffffu) {
        pci_iounmap(pdev, state->registers);
        kfree(state);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -EIO;
    }

    pci_set_drvdata(pdev, state);
    pr_info("qemu-edu: Linux-style driver bound to %s; BAR0=%p len=%llu identity=0x%x",
            dev_name(&pdev->dev),
            state->registers,
            (unsigned long long)pci_resource_len(pdev, 0),
            state->identity);
    return 0;
}

static void twilight_edu_remove(struct pci_dev *pdev) {
    struct edu_state *state = (struct edu_state *)pci_get_drvdata(pdev);
    if (state != 0) {
        if (state->registers != 0) pci_iounmap(pdev, state->registers);
        kfree(state);
    }

    pci_set_drvdata(pdev, 0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pr_info("qemu-edu: driver removed from %s", dev_name(&pdev->dev));
}

static const struct pci_device_id twilight_edu_ids[] = {
    { PCI_DEVICE(QEMU_EDU_VENDOR_ID, QEMU_EDU_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, twilight_edu_ids);

static struct pci_driver twilight_edu_driver = {
    .name = "twilight-linux-edu",
    .id_table = twilight_edu_ids,
    .probe = twilight_edu_probe,
    .remove = twilight_edu_remove,
};

module_pci_driver(twilight_edu_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Twilight compatibility bring-up");
MODULE_DESCRIPTION("QEMU EDU proof driver using Twilight's Linux PCI compatibility layer");
