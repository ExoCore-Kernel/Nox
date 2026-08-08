#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/ata.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/libata.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <twilight/block.h>
#include <twilight/linux_storage.h>

#ifndef TWILIGHT_STORAGE_SELF_TEST
#define TWILIGHT_STORAGE_SELF_TEST 0
#endif

#define AHCI_TEST_BAR 5
#define AHCI_HOST_IRQ_STAT 0x08u
#define AHCI_PORT_CMD_ISSUE 0x38u
#define ATA_BLOCK_MAX_SECTORS_PER_IO 128u
#define ATA_BLOCK_SELF_TEST_LBA 128u

struct ata_block_context {
    struct ata_port *ap;
    struct ata_device *dev;
};

static void clear_ahci_irq_for_poll(struct ata_port *ap) {
    if (ap == 0) return;
    if (ap->ops != 0 && ap->ops->irq_clear != 0) ap->ops->irq_clear(ap);
    if (ap->host != 0 && ap->host->iomap != 0 && ap->host->iomap[AHCI_TEST_BAR] != 0) {
        writel(1u << ap->port_no,
               (u8 __iomem *)ap->host->iomap[AHCI_TEST_BAR] + AHCI_HOST_IRQ_STAT);
    }
}

static int issue_polled_qc(struct ata_port *ap,
                           struct ata_device *dev,
                           struct ata_taskfile *tf,
                           void *buffer,
                           dma_addr_t dma,
                           unsigned int bytes) {
    if (ap == 0 || dev == 0 || tf == 0 || buffer == 0 || bytes == 0 ||
        ap->ops == 0 || ap->ops->qc_prep == 0 || ap->ops->qc_issue == 0 ||
        ap->ioaddr.cmd_addr == 0) {
        return -EINVAL;
    }

    struct scatterlist sg = {
        .address = buffer,
        .length = bytes,
        .dma_address = dma,
        .dma_length = bytes,
    };

    struct ata_queued_cmd *qc = &ap->qcmd[0];
    *qc = (struct ata_queued_cmd){0};
    qc->ap = ap;
    qc->dev = dev;
    qc->tf = *tf;
    qc->tag = 0;
    qc->flags = ATA_QCFLAG_ACTIVE | ATA_QCFLAG_SG;
    qc->n_elem = 1;
    qc->orig_n_elem = 1;
    qc->__sg = &sg;
    qc->buf_virt = buffer;
    qc->nbytes = bytes;

    ap->link.active_tag = 0;
    ap->qc_active = 1u;

    /* qc_prep/qc_issue are the callbacks installed by the unmodified Linux
     * ahci.c. This bridge never programs command headers, FISes or PxCI itself. */
    ap->ops->qc_prep(qc);

    unsigned long irq_flags;
    local_irq_save(irq_flags);
    clear_ahci_irq_for_poll(ap);
    const unsigned int issue_rc = ap->ops->qc_issue(qc);
    if (issue_rc != 0) {
        ap->qc_active = 0;
        ap->link.active_tag = ATA_TAG_POISON;
        local_irq_restore(irq_flags);
        return -EIO;
    }

    bool complete = false;
    for (unsigned int attempt = 0; attempt < 20000u; ++attempt) {
        const u32 active = readl((u8 __iomem *)ap->ioaddr.cmd_addr + AHCI_PORT_CMD_ISSUE);
        if ((active & 1u) == 0) {
            complete = true;
            break;
        }
        udelay(50);
    }

    clear_ahci_irq_for_poll(ap);
    ap->qc_active = 0;
    ap->link.active_tag = ATA_TAG_POISON;
    local_irq_restore(irq_flags);

    if (!complete) return -ETIMEDOUT;
    if (ap->ops->check_status != 0) {
        const u8 status = ap->ops->check_status(ap);
        if ((status & (ATA_BUSY | ATA_DRQ | ATA_ERR)) != 0) return -EIO;
    }
    return 0;
}

static void prepare_rw_tf(struct ata_taskfile *tf,
                          uint64_t lba,
                          uint32_t sectors,
                          bool write) {
    *tf = (struct ata_taskfile){0};
    tf->flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE | ATA_TFLAG_LBA |
                ATA_TFLAG_LBA48 | (write ? ATA_TFLAG_WRITE : 0u);
    tf->protocol = ATA_PROT_DMA;
    tf->device = ATA_DEVICE_OBS | (1u << 6);
    tf->nsect = (u8)(sectors & 0xffu);
    tf->hob_nsect = (u8)((sectors >> 8) & 0xffu);
    tf->lbal = (u8)(lba & 0xffu);
    tf->lbam = (u8)((lba >> 8) & 0xffu);
    tf->lbah = (u8)((lba >> 16) & 0xffu);
    tf->hob_lbal = (u8)((lba >> 24) & 0xffu);
    tf->hob_lbam = (u8)((lba >> 32) & 0xffu);
    tf->hob_lbah = (u8)((lba >> 40) & 0xffu);
    tf->command = write ? ATA_CMD_WRITE_EXT : ATA_CMD_READ_EXT;
}

static int ata_block_transfer(struct ata_block_context *context,
                              uint64_t lba,
                              uint32_t sectors,
                              void *buffer,
                              bool write) {
    if (context == 0 || context->ap == 0 || context->dev == 0 ||
        buffer == 0 || sectors == 0 || sectors > ATA_BLOCK_MAX_SECTORS_PER_IO ||
        lba > 0x0000ffffffffffffull) {
        return -EINVAL;
    }

    const size_t bytes = (size_t)sectors * ATA_SECT_SIZE;
    dma_addr_t dma = DMA_MAPPING_ERROR;
    void *dma_buffer = dma_alloc_coherent(context->ap->host->dev, bytes, &dma, GFP_KERNEL);
    if (dma_buffer == 0) return -ENOMEM;

    if (write) memcpy(dma_buffer, buffer, bytes);
    else memset(dma_buffer, 0, bytes);

    struct ata_taskfile tf;
    prepare_rw_tf(&tf, lba, sectors, write);
    const int rc = issue_polled_qc(context->ap, context->dev, &tf,
                                   dma_buffer, dma, (unsigned int)bytes);
    if (rc == 0 && !write) memcpy(buffer, dma_buffer, bytes);

    dma_free_coherent(context->ap->host->dev, bytes, dma_buffer, dma);
    return rc;
}

static int ata_block_read(void *opaque,
                          uint64_t lba,
                          uint32_t sectors,
                          void *buffer) {
    return ata_block_transfer((struct ata_block_context *)opaque,
                              lba, sectors, buffer, false);
}

static int ata_block_write(void *opaque,
                           uint64_t lba,
                           uint32_t sectors,
                           const void *buffer) {
    return ata_block_transfer((struct ata_block_context *)opaque,
                              lba, sectors, (void *)buffer, true);
}

static const struct twilight_block_ops ata_block_ops = {
    .read = ata_block_read,
    .write = ata_block_write,
};

static uint64_t identify_sector_count(const u16 *id) {
    if (id == 0) return 0;

    const uint64_t lba48 = (uint64_t)id[100] |
                           ((uint64_t)id[101] << 16) |
                           ((uint64_t)id[102] << 32) |
                           ((uint64_t)id[103] << 48);
    if ((id[83] & (1u << 10)) != 0 && lba48 != 0) return lba48;

    return (uint64_t)id[60] | ((uint64_t)id[61] << 16);
}

static void identify_model(const u16 *id, char out[41]) {
    if (out == 0) return;
    if (id == 0) {
        out[0] = '\0';
        return;
    }

    for (unsigned int word = 0; word < 20u; ++word) {
        const u16 value = id[27u + word];
        out[word * 2u] = (char)(value >> 8);
        out[word * 2u + 1u] = (char)(value & 0xffu);
    }
    out[40] = '\0';
    for (int i = 39; i >= 0 && out[i] == ' '; --i) out[i] = '\0';
}

#if TWILIGHT_STORAGE_SELF_TEST
static bool buffers_equal(const u8 *a, const u8 *b, size_t bytes) {
    for (size_t i = 0; i < bytes; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static bool run_block_rw_self_test(struct twilight_block_device *device) {
    if (device == 0 || device->sector_count <= ATA_BLOCK_SELF_TEST_LBA) return false;

    u8 original[ATA_SECT_SIZE];
    u8 pattern[ATA_SECT_SIZE];
    u8 verify[ATA_SECT_SIZE];

    if (block_read(device, ATA_BLOCK_SELF_TEST_LBA, 1, original) != 0) {
        printk("[block:error] %s self-test could not read LBA %u",
               device->name, ATA_BLOCK_SELF_TEST_LBA);
        return false;
    }

    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = (u8)(i ^ 0xa5u);
    const char marker[] = "NOX BLOCK WRITE/READ SELFTEST";
    memcpy(pattern, marker, sizeof(marker));

    if (block_write(device, ATA_BLOCK_SELF_TEST_LBA, 1, pattern) != 0) {
        printk("[block:error] %s self-test could not write LBA %u",
               device->name, ATA_BLOCK_SELF_TEST_LBA);
        return false;
    }

    memset(verify, 0, sizeof(verify));
    const int read_rc = block_read(device, ATA_BLOCK_SELF_TEST_LBA, 1, verify);
    const bool matched = read_rc == 0 && buffers_equal(pattern, verify, sizeof(pattern));

    /* Restore the sector even on a failed verification. This test is enabled
     * only for the disposable QEMU image, but keeping restoration semantics
     * makes the block layer safer to reuse. */
    const int restore_rc = block_write(device, ATA_BLOCK_SELF_TEST_LBA, 1, original);

    if (!matched) {
        printk("[block:error] %s LBA %u write/read verification FAILED",
               device->name, ATA_BLOCK_SELF_TEST_LBA);
        return false;
    }
    if (restore_rc != 0) {
        printk("[block:error] %s LBA %u verified but original sector restore failed",
               device->name, ATA_BLOCK_SELF_TEST_LBA);
        return false;
    }

    printk("[block] %s LBA %u WRITE + READ + RESTORE PASS through unmodified Linux AHCI datapath",
           device->name, ATA_BLOCK_SELF_TEST_LBA);
    return true;
}
#endif

bool linux_storage_publish_block_devices(void) {
    bool published_any = false;
    unsigned int disk_index = 0;

    for (size_t pci_index = 0; pci_index < linux_pci_device_count(); ++pci_index) {
        struct pci_dev *pdev = linux_pci_device_at(pci_index);
        if (pdev == 0 || (pdev->class & 0x00ffffffu) != PCI_CLASS_STORAGE_SATA_AHCI) continue;

        struct ata_host *host = (struct ata_host *)dev_get_drvdata(&pdev->dev);
        if (host == 0) continue;

        for (unsigned int port = 0; port < host->n_ports; ++port) {
            struct ata_port *ap = host->ports[port];
            if (ata_port_is_dummy(ap) || ap->ioaddr.cmd_addr == 0) continue;

            struct ata_device *dev = &ap->link.device[0];
            if (dev->class != ATA_DEV_ATA) continue;

            const uint64_t sectors = identify_sector_count(dev->id);
            if (sectors == 0) continue;
            dev->n_sectors = sectors;

            struct ata_block_context *context = kzalloc(sizeof(*context), GFP_KERNEL);
            if (context == 0) return published_any;
            context->ap = ap;
            context->dev = dev;

            char name[4] = {'s', 'd', (char)('a' + disk_index), '\0'};
            char model[41];
            identify_model(dev->id, model);

            struct twilight_block_device *block_device = 0;
            if (!block_register_device(name,
                                       model[0] != '\0' ? model : "ATA disk",
                                       sectors,
                                       ATA_SECT_SIZE,
                                       true,
                                       &ata_block_ops,
                                       context,
                                       &block_device)) {
                kfree(context);
                continue;
            }

            ++disk_index;
            published_any = true;
            printk("[linux:ata] ata%u published as %s (%llu KiB)",
                   ap->print_id,
                   block_device->name,
                   (unsigned long long)((sectors * ATA_SECT_SIZE) / 1024u));

#if TWILIGHT_STORAGE_SELF_TEST
            if (!run_block_rw_self_test(block_device)) {
                printk("[block:error] %s storage self-test failed", block_device->name);
            }
#endif
        }
    }

    return published_any;
}
