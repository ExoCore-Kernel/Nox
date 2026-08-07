#include <linux/pci.h>

const char *pci_name(const struct pci_dev *pdev) {
    if (pdev == 0 || pdev->dev.init_name == 0) return "pci-device";
    return pdev->dev.init_name;
}
