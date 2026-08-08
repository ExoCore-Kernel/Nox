#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/libata.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/string.h>

#define AHCI_TEST_BAR 5
#define AHCI_HOST_IRQ_STAT 0x08u
#define AHCI_PORT_CMD_ISSUE 0x38u
#define AHCI_PORT_SSTATUS_DET_MASK 0x0fu
#define AHCI_PORT_SSTATUS_DET_PRESENT 0x03u

struct class_device_attribute class_device_attr_link_power_management_policy = {
    .name = "link_power_management_policy",
};

static void dummy_port_stop(struct ata_port *ap) { (void)ap; }
const struct ata_port_operations ata_dummy_port_ops = {
    .port_stop = dummy_port_stop,
};
const struct ata_port_info ata_dummy_port_info = {
    .port_ops = &ata_dummy_port_ops,
};

static void init_port(struct ata_host *host,
                      struct ata_port *ap,
                      const struct ata_port_info *pi,
                      unsigned int port_no) {
    *ap = (struct ata_port){0};
    ap->host = host;
    ap->dev = host->dev;
    ap->lock = &host->lock;
    ap->port_no = port_no;
    ap->print_id = port_no;
    ap->ops = pi != 0 && pi->port_ops != 0 ? pi->port_ops : &ata_dummy_port_ops;
    ap->flags = pi != 0 ? pi->flags : 0;
    ap->pio_mask = pi != 0 ? (unsigned int)pi->pio_mask : 0;
    ap->mwdma_mask = pi != 0 ? (unsigned int)pi->mwdma_mask : 0;
    ap->udma_mask = pi != 0 ? (unsigned int)pi->udma_mask : 0;
    ap->cbl = ATA_CBL_SATA;
    ap->link.ap = ap;
    ap->link.pmp = 0;
    ap->link.active_tag = ATA_TAG_POISON;
    ap->link.flags = pi != 0 ? (unsigned int)pi->link_flags : 0;
    for (unsigned int devno = 0; devno < ATA_MAX_DEVICES; ++devno) {
        ap->link.device[devno].link = &ap->link;
        ap->link.device[devno].devno = devno;
        ap->link.device[devno].class = ATA_DEV_UNKNOWN;
        ap->link.device[devno].cdb_len = ATAPI_CDB_LEN;
    }
    for (unsigned int tag = 0; tag < ATA_MAX_QUEUE; ++tag) {
        ap->qcmd[tag].ap = ap;
        ap->qcmd[tag].tag = tag;
    }
}

struct ata_host *ata_host_alloc_pinfo(struct device *dev,
                                      const struct ata_port_info * const *ppi,
                                      int n_ports) {
    if (dev == 0 || ppi == 0 || ppi[0] == 0 || n_ports <= 0) return 0;

    struct ata_host *host = kzalloc(sizeof(*host), GFP_KERNEL);
    if (host == 0) return 0;

    host->ports = kcalloc((size_t)n_ports, sizeof(*host->ports), GFP_KERNEL);
    if (host->ports == 0) {
        kfree(host);
        return 0;
    }

    host->dev = dev;
    host->n_ports = (unsigned int)n_ports;
    host->ops = ppi[0]->port_ops;
    spin_lock_init(&host->lock);

    /* The v2.6 libata convention used by ahci.c is ppi={&pi,NULL}: the final
     * non-NULL port-info is reused for all remaining ports. AHCI has one
     * homogeneous port-info, so entry zero is the correct template for every
     * port and avoids reading beyond the sentinel array. */
    for (int i = 0; i < n_ports; ++i) {
        host->ports[i] = kzalloc(sizeof(*host->ports[i]), GFP_KERNEL);
        if (host->ports[i] == 0) {
            ata_host_detach(host);
            return 0;
        }
        init_port(host, host->ports[i], ppi[0], (unsigned int)i);
    }

    return host;
}

static bool port_link_present(struct ata_port *ap) {
    if (ap == 0 || ap->ops == 0 || ap->ops->scr_read == 0) return false;
    u32 sstatus = 0;
    if (ap->ops->scr_read(ap, SCR_STATUS, &sstatus) != 0) return false;
    return (sstatus & AHCI_PORT_SSTATUS_DET_MASK) == AHCI_PORT_SSTATUS_DET_PRESENT;
}

static void clear_ahci_irq_for_poll(struct ata_port *ap) {
    if (ap == 0) return;
    if (ap->ops != 0 && ap->ops->irq_clear != 0) ap->ops->irq_clear(ap);
    if (ap->host != 0 && ap->host->iomap != 0 && ap->host->iomap[AHCI_TEST_BAR] != 0)
        writel(1u << ap->port_no,
               (u8 __iomem *)ap->host->iomap[AHCI_TEST_BAR] + AHCI_HOST_IRQ_STAT);
}

static int issue_polled_qc(struct ata_port *ap,
                           struct ata_device *dev,
                           struct ata_taskfile *tf,
                           void *buffer,
                           dma_addr_t dma,
                           unsigned int bytes) {
    if (ap == 0 || dev == 0 || tf == 0 || buffer == 0 || bytes == 0 ||
        ap->ops == 0 || ap->ops->qc_prep == 0 || ap->ops->qc_issue == 0 ||
        ap->ioaddr.cmd_addr == 0) return -EINVAL;

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

    /* These callbacks are the functions from the untouched Linux ahci.c:
     * ahci_qc_prep() creates the H2D FIS/PRDT and ahci_qc_issue() writes PxCI. */
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

static void prepare_identify_tf(struct ata_taskfile *tf) {
    *tf = (struct ata_taskfile){0};
    tf->protocol = ATA_PROT_PIO;
    tf->device = ATA_DEVICE_OBS;
    tf->command = ATA_CMD_ID_ATA;
}

static void prepare_read_lba0_tf(struct ata_taskfile *tf) {
    *tf = (struct ata_taskfile){0};
    tf->flags = ATA_TFLAG_ISADDR | ATA_TFLAG_DEVICE | ATA_TFLAG_LBA |
                ATA_TFLAG_LBA48;
    tf->protocol = ATA_PROT_DMA;
    tf->device = ATA_DEVICE_OBS | (1u << 6);
    tf->nsect = 1;
    tf->command = ATA_CMD_READ_EXT;
}

static void ata_model_string(const u16 *id, char out[41]) {
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

static int probe_and_read_port(struct ata_port *ap) {
    if (ap == 0 || !port_link_present(ap)) return -ENODEV;

    struct ata_device *dev = &ap->link.device[0];
    dev->class = ATA_DEV_ATA;

    dma_addr_t dma = DMA_MAPPING_ERROR;
    u8 *sector = dma_alloc_coherent(ap->host->dev, ATA_SECT_SIZE, &dma, GFP_KERNEL);
    if (sector == 0) return -ENOMEM;

    memset(sector, 0, ATA_SECT_SIZE);
    struct ata_taskfile tf;
    prepare_identify_tf(&tf);
    int rc = issue_polled_qc(ap, dev, &tf, sector, dma, ATA_SECT_SIZE);
    if (rc != 0) {
        ata_port_printk(ap, KERN_ERR, "IDENTIFY DEVICE failed (%d)\n", rc);
        dma_free_coherent(ap->host->dev, ATA_SECT_SIZE, sector, dma);
        return rc;
    }

    memcpy(dev->id, sector, ATA_SECT_SIZE);
    char model[41];
    ata_model_string(dev->id, model);
    ata_port_printk(ap, KERN_INFO, "SATA disk identified: %s\n",
                    model[0] != '\0' ? model : "unknown model");

    memset(sector, 0, ATA_SECT_SIZE);
    prepare_read_lba0_tf(&tf);
    rc = issue_polled_qc(ap, dev, &tf, sector, dma, ATA_SECT_SIZE);
    if (rc == 0) {
        if (sector[0] == 'N' && sector[1] == 'O' && sector[2] == 'X' &&
            sector[3] == 'A' && sector[4] == 'H' && sector[5] == 'C' &&
            sector[6] == 'I') {
            printk("[linux:ata] ata%u: AHCI LBA0 READ PASS: unmodified Linux ahci.c transferred the NOXAHCI marker through SATA DMA",
                   ap->print_id);
        } else {
            printk("[linux:ata] ata%u: AHCI LBA0 read completed, but test marker did not match",
                   ap->print_id);
            rc = -EIO;
        }
    } else {
        ata_port_printk(ap, KERN_ERR, "READ DMA EXT LBA0 failed (%d)\n", rc);
    }

    dma_free_coherent(ap->host->dev, ATA_SECT_SIZE, sector, dma);
    return rc;
}

int ata_host_activate(struct ata_host *host, int irq,
                      irq_handler_t irq_handler, unsigned long irq_flags,
                      struct scsi_host_template *sht) {
    (void)sht;
    if (host == 0 || irq_handler == 0 || irq < 0) return -EINVAL;

    for (unsigned int i = 0; i < host->n_ports; ++i) {
        struct ata_port *ap = host->ports[i];
        if (ata_port_is_dummy(ap) || ap->ioaddr.cmd_addr == 0) continue;
        if (ap->ops->port_start != 0) {
            const int rc = ap->ops->port_start(ap);
            if (rc != 0) {
                ata_port_printk(ap, KERN_ERR, "low-level port_start failed (%d)\n", rc);
                return rc;
            }
        }
    }

    const int irq_rc = request_irq((unsigned int)irq, irq_handler, irq_flags,
                                   "ahci", host);
    if (irq_rc != 0) return irq_rc;

    dev_set_drvdata(host->dev, host);
    printk("[linux:ata] AHCI libata compatibility host activated on IRQ %d with %u port(s)",
           irq, host->n_ports);

    int first_result = -ENODEV;
    for (unsigned int i = 0; i < host->n_ports; ++i) {
        struct ata_port *ap = host->ports[i];
        if (ata_port_is_dummy(ap) || ap->ioaddr.cmd_addr == 0) continue;
        const int rc = probe_and_read_port(ap);
        if (rc == 0) return 0;
        if (rc != -ENODEV && first_result == -ENODEV) first_result = rc;
    }

    if (first_result == -ENODEV)
        printk("[linux:ata] AHCI controller initialized, but no SATA device reported PHY present");
    free_irq((unsigned int)irq, host);
    return first_result;
}

void ata_host_detach(struct ata_host *host) {
    if (host == 0) return;
    if (host->ports != 0) {
        for (unsigned int i = 0; i < host->n_ports; ++i) {
            struct ata_port *ap = host->ports[i];
            if (ap == 0) continue;
            if (ap->ops != 0 && ap->ops->port_stop != 0) ap->ops->port_stop(ap);
            kfree(ap);
        }
        kfree(host->ports);
    }
    kfree(host);
}

void ata_pci_remove_one(struct pci_dev *pdev) {
    if (pdev == 0) return;
    struct ata_host *host = dev_get_drvdata(&pdev->dev);
    if (host != 0) ata_host_detach(host);
    dev_set_drvdata(&pdev->dev, 0);
}

int ata_pad_alloc(struct ata_port *ap, struct device *dev) {
    (void)ap;
    (void)dev;
    return 0;
}

void ata_port_pbar_desc(struct ata_port *ap, int bar, int offset, const char *name) {
    (void)ap; (void)bar; (void)offset; (void)name;
}

u32 ata_wait_register(void __iomem *reg, u32 mask, u32 val,
                      unsigned long interval_msec, unsigned long timeout_msec) {
    if (reg == 0) return ~0u;
    const unsigned long interval = interval_msec == 0 ? 1 : interval_msec;
    unsigned long waited = 0;
    u32 value = readl(reg);
    while ((value & mask) == val && waited < timeout_msec) {
        mdelay(interval);
        waited += interval;
        value = readl(reg);
    }
    return value;
}

void ata_wait_after_reset(struct ata_port *ap, unsigned long deadline) {
    (void)ap; (void)deadline;
    msleep(10);
}

int ata_wait_ready(struct ata_port *ap, unsigned long deadline) {
    if (ap == 0 || ap->ops == 0 || ap->ops->check_status == 0) return -EINVAL;
    while (time_before(jiffies, deadline)) {
        const u8 status = ap->ops->check_status(ap);
        if ((status & ATA_BUSY) == 0) return 0;
        msleep(1);
    }
    return -ETIMEDOUT;
}

void ata_tf_init(struct ata_device *dev, struct ata_taskfile *tf) {
    (void)dev;
    if (tf == 0) return;
    *tf = (struct ata_taskfile){0};
    tf->device = ATA_DEVICE_OBS;
}

void ata_tf_to_fis(const struct ata_taskfile *tf, u8 pmp, int is_cmd, u8 *fis) {
    if (tf == 0 || fis == 0) return;
    memset(fis, 0, 20);
    fis[0] = 0x27;
    fis[1] = (u8)((pmp & 0x0fu) | (is_cmd ? 0x80u : 0u));
    fis[2] = tf->command;
    fis[3] = tf->feature;
    fis[4] = tf->lbal;
    fis[5] = tf->lbam;
    fis[6] = tf->lbah;
    fis[7] = tf->device;
    fis[8] = tf->hob_lbal;
    fis[9] = tf->hob_lbam;
    fis[10] = tf->hob_lbah;
    fis[11] = tf->hob_feature;
    fis[12] = tf->nsect;
    fis[13] = tf->hob_nsect;
    fis[15] = tf->ctl;
}

void ata_tf_from_fis(const u8 *fis, struct ata_taskfile *tf) {
    if (fis == 0 || tf == 0) return;
    tf->command = fis[2];
    tf->feature = fis[3];
    tf->lbal = fis[4];
    tf->lbam = fis[5];
    tf->lbah = fis[6];
    tf->device = fis[7];
    tf->hob_lbal = fis[8];
    tf->hob_lbam = fis[9];
    tf->hob_lbah = fis[10];
    tf->hob_feature = fis[11];
    tf->nsect = fis[12];
    tf->hob_nsect = fis[13];
}

unsigned int ata_dev_classify(const struct ata_taskfile *tf) {
    if (tf == 0) return ATA_DEV_UNKNOWN;
    if (tf->lbam == 0x00 && tf->lbah == 0x00) return ATA_DEV_ATA;
    if (tf->lbam == 0x14 && tf->lbah == 0xeb) return ATA_DEV_ATAPI;
    if (tf->lbam == 0x69 && tf->lbah == 0x96) return ATA_DEV_PMP;
    return ATA_DEV_UNKNOWN;
}

void ata_noop_dev_select(struct ata_port *ap, unsigned int device) {
    (void)ap; (void)device;
}

int sata_pmp_qc_defer_cmd_switch(struct ata_queued_cmd *qc) {
    (void)qc;
    return 0;
}

int ata_qc_complete_multiple(struct ata_port *ap, u32 qc_active, void *finish_qc) {
    (void)finish_qc;
    if (ap == 0) return -EINVAL;
    const u32 completed = ap->qc_active & ~qc_active;
    ap->qc_active = qc_active;
    if (completed != 0 && qc_active == 0) ap->link.active_tag = ATA_TAG_POISON;
    return (int)__builtin_popcount(completed);
}

void ata_port_abort(struct ata_port *ap) {
    if (ap != 0) ap->qc_active = 0;
}

void ata_port_freeze(struct ata_port *ap) {
    if (ap != 0) ap->pflags |= ATA_PFLAG_FROZEN;
}

void ata_ehi_clear_desc(struct ata_eh_info *ehi) {
    if (ehi == 0) return;
    ehi->desc_len = 0;
    ehi->desc[0] = '\0';
}

void ata_ehi_push_desc(struct ata_eh_info *ehi, const char *fmt, ...) {
    (void)fmt;
    if (ehi == 0) return;
    if (ehi->desc_len == 0) {
        const char text[] = "AHCI error";
        memcpy(ehi->desc, text, sizeof(text));
        ehi->desc_len = (int)(sizeof(text) - 1u);
    }
}

void ata_ehi_hotplugged(struct ata_eh_info *ehi) {
    if (ehi != 0) ehi->action |= ATA_EH_SOFTRESET;
}

void sata_async_notification(struct ata_port *ap) { (void)ap; }

int ata_link_offline(struct ata_link *link) { return !ata_link_online(link); }

int ata_link_online(struct ata_link *link) {
    return link != 0 && port_link_present(link->ap);
}

int ata_std_prereset(struct ata_link *link, unsigned long deadline) {
    (void)deadline;
    return ata_link_online(link) ? 0 : -ENODEV;
}

int sata_std_hardreset(struct ata_link *link, unsigned int *class, unsigned long deadline) {
    (void)deadline;
    if (class != 0) *class = ata_link_online(link) ? ATA_DEV_ATA : ATA_DEV_NONE;
    return 0;
}

int sata_link_hardreset(struct ata_link *link, const unsigned long *timing, unsigned long deadline) {
    (void)timing; (void)deadline;
    return ata_link_online(link) ? 0 : -ENODEV;
}

const unsigned long *sata_ehc_deb_timing(struct ata_eh_context *ehc) {
    static const unsigned long timing[] = { 5, 100, 2000, 0 };
    (void)ehc;
    return timing;
}

void ata_std_postreset(struct ata_link *link, unsigned int *classes) {
    (void)link; (void)classes;
}

void sata_pmp_error_handler(struct ata_port *ap) { (void)ap; }
int sata_pmp_std_prereset(struct ata_link *link, unsigned long deadline) {
    return ata_std_prereset(link, deadline);
}
int sata_pmp_std_hardreset(struct ata_link *link, unsigned int *class, unsigned long deadline) {
    return sata_std_hardreset(link, class, deadline);
}
void sata_pmp_std_postreset(struct ata_link *link, unsigned int *classes) {
    ata_std_postreset(link, classes);
}
void ata_std_error_handler(struct ata_port *ap) { (void)ap; }

void ata_do_eh(struct ata_port *ap, ata_prereset_fn_t prereset,
               ata_reset_fn_t softreset, ata_reset_fn_t hardreset,
               ata_postreset_fn_t postreset) {
    if (ap == 0) return;
    unsigned int class = ATA_DEV_UNKNOWN;
    const unsigned long deadline = jiffies + 1000ul;
    if (prereset != 0 && prereset(&ap->link, deadline) != 0) return;
    int rc = hardreset != 0 ? hardreset(&ap->link, &class, deadline) : -ENOSYS;
    if (rc != 0 && softreset != 0) rc = softreset(&ap->link, &class, deadline);
    if (rc == 0 && postreset != 0) postreset(&ap->link, &class);
    ap->pflags &= ~ATA_PFLAG_FROZEN;
}

void sata_pmp_do_eh(struct ata_port *ap,
                    ata_prereset_fn_t prereset, ata_reset_fn_t softreset,
                    ata_reset_fn_t hardreset, ata_postreset_fn_t postreset,
                    ata_prereset_fn_t pmp_prereset, ata_reset_fn_t pmp_softreset,
                    ata_reset_fn_t pmp_hardreset, ata_postreset_fn_t pmp_postreset) {
    (void)pmp_prereset; (void)pmp_softreset; (void)pmp_hardreset; (void)pmp_postreset;
    ata_do_eh(ap, prereset, softreset, hardreset, postreset);
}

int ata_scsi_ioctl(struct scsi_device *dev, int cmd, void *arg) {
    (void)dev; (void)cmd; (void)arg; return -ENOSYS;
}
int ata_scsi_queuecmd(struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *)) {
    (void)cmd; (void)done; return -ENOSYS;
}
int ata_scsi_change_queue_depth(struct scsi_device *sdev, int queue_depth) {
    (void)sdev; return queue_depth;
}
int ata_scsi_slave_config(struct scsi_device *sdev) { (void)sdev; return 0; }
void ata_scsi_slave_destroy(struct scsi_device *sdev) { (void)sdev; }
int ata_std_bios_param(struct scsi_device *sdev, struct block_device *bdev,
                       sector_t capacity, int *geom) {
    (void)sdev; (void)bdev; (void)capacity; (void)geom; return 0;
}
