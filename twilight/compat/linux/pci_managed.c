#include <linux/device.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/slab.h>

struct pcim_mapping {
    struct pci_dev *pdev;
    void __iomem *address;
};

static void pcim_disable_action(void *data) {
    struct pci_dev *pdev = (struct pci_dev *)data;
    pci_disable_device(pdev);
}

static void pcim_unmap_action(void *data) {
    struct pcim_mapping *mapping = (struct pcim_mapping *)data;
    if (mapping != 0 && mapping->pdev != 0 && mapping->address != 0)
        pci_iounmap(mapping->pdev, mapping->address);
}

int pcim_enable_device(struct pci_dev *pdev) {
    if (pdev == 0) return -ENODEV;
    const int result = pci_enable_device(pdev);
    if (result != 0) return result;
    return devm_add_action_or_reset(&pdev->dev, pcim_disable_action, pdev);
}

void __iomem *pcim_iomap(struct pci_dev *pdev, int bar, unsigned long maxlen) {
    if (pdev == 0) return 0;
    void __iomem *address = pci_iomap(pdev, bar, maxlen);
    if (address == 0) return 0;

    struct pcim_mapping *mapping =
        (struct pcim_mapping *)devm_kmalloc(&pdev->dev, sizeof(*mapping), GFP_KERNEL);
    if (mapping == 0) {
        pci_iounmap(pdev, address);
        return 0;
    }
    mapping->pdev = pdev;
    mapping->address = address;

    if (devm_add_action_or_reset(&pdev->dev, pcim_unmap_action, mapping) != 0)
        return 0;
    return address;
}
