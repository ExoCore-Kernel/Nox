#include <stdbool.h>
#include <stdint.h>

#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/apic.h>
#include <twilight/irq.h>

#define X86_MSI_ADDRESS_BASE 0xfee00000u

static int pci_msi_write_message(struct pci_dev *pdev,
                                 int capability,
                                 u16 flags,
                                 uint8_t vector) {
    const u32 address_low = X86_MSI_ADDRESS_BASE | ((u32)apic_id() << 12);
    if (pci_write_config_dword(pdev, capability + PCI_MSI_ADDRESS_LO,
                               address_low) != PCIBIOS_SUCCESSFUL)
        return -EIO;

    if ((flags & PCI_MSI_FLAGS_64BIT) != 0) {
        if (pci_write_config_dword(pdev, capability + PCI_MSI_ADDRESS_HI, 0u) !=
            PCIBIOS_SUCCESSFUL)
            return -EIO;
        if (pci_write_config_word(pdev, capability + PCI_MSI_DATA_64,
                                  (u16)vector) != PCIBIOS_SUCCESSFUL)
            return -EIO;
    } else {
        if (pci_write_config_word(pdev, capability + PCI_MSI_DATA_32,
                                  (u16)vector) != PCIBIOS_SUCCESSFUL)
            return -EIO;
    }

    /* If the function implements per-vector masking, ensure message zero is
     * unmasked. Twilight currently enables one MSI message per function. */
    if ((flags & PCI_MSI_FLAGS_MASKBIT) != 0) {
        const int mask_offset = capability +
            (((flags & PCI_MSI_FLAGS_64BIT) != 0) ? PCI_MSI_MASK_64 : PCI_MSI_MASK_32);
        u32 mask = 0;
        if (pci_read_config_dword(pdev, mask_offset, &mask) != PCIBIOS_SUCCESSFUL)
            return -EIO;
        mask &= ~1u;
        if (pci_write_config_dword(pdev, mask_offset, mask) != PCIBIOS_SUCCESSFUL)
            return -EIO;
    }

    return 0;
}

int pci_enable_msi(struct pci_dev *pdev) {
    if (pdev == 0 || pdev->twilight == 0) return -ENODEV;
    if (pdev->msi_enabled) return 0;

    if (!apic_native_enabled() && !apic_enable_native()) return -ENODEV;
    if (!irq_core_is_initialized()) return -ENODEV;

    const int capability = pci_find_capability(pdev, PCI_CAP_ID_MSI);
    if (capability == 0) return -ENODEV;

    u16 flags = 0;
    if (pci_read_config_word(pdev, capability + PCI_MSI_FLAGS, &flags) !=
        PCIBIOS_SUCCESSFUL)
        return -EIO;

    uint8_t vector = 0;
    if (!irq_allocate_msi_vector(&vector)) return -ENOSPC;

    const int message_result = pci_msi_write_message(pdev, capability, flags, vector);
    if (message_result != 0) {
        irq_release_msi_vector(vector);
        return message_result;
    }

    /* Multiple-message enable is deliberately kept at one vector. Drivers can
     * use MSI-X later when the generic vector-array API is available. */
    u16 enabled_flags = (u16)(flags & (u16)~PCI_MSI_FLAGS_QSIZE);
    enabled_flags |= PCI_MSI_FLAGS_ENABLE;
    if (pci_write_config_word(pdev, capability + PCI_MSI_FLAGS, enabled_flags) !=
        PCIBIOS_SUCCESSFUL) {
        irq_release_msi_vector(vector);
        return -EIO;
    }

    pdev->legacy_irq = pdev->irq;
    pdev->msi_cap = (u8)capability;
    pdev->msi_vector = vector;
    pdev->msi_enabled = true;
    pdev->irq = vector;

    /* Avoid a duplicate legacy INTx interrupt while MSI is active. */
    pci_intx(pdev, 0);

    pr_info("PCI MSI enabled for %s: LAPIC %u vector 0x%02x (legacy IRQ %u)",
            pci_name(pdev), (unsigned)apic_id(), (unsigned)vector,
            pdev->legacy_irq);
    return 0;
}

void pci_disable_msi(struct pci_dev *pdev) {
    if (pdev == 0 || !pdev->msi_enabled) return;

    u16 flags = 0;
    if (pdev->msi_cap != 0 &&
        pci_read_config_word(pdev, pdev->msi_cap + PCI_MSI_FLAGS, &flags) ==
            PCIBIOS_SUCCESSFUL) {
        flags &= (u16)~PCI_MSI_FLAGS_ENABLE;
        (void)pci_write_config_word(pdev, pdev->msi_cap + PCI_MSI_FLAGS, flags);
    }

    const uint8_t vector = pdev->msi_vector;
    pdev->irq = pdev->legacy_irq;
    pdev->msi_enabled = false;
    pdev->msi_cap = 0;
    pdev->msi_vector = 0;

    pci_intx(pdev, 1);
    irq_release_msi_vector(vector);

    pr_info("PCI MSI disabled for %s; restored legacy IRQ %u",
            pci_name(pdev), pdev->irq);
}
