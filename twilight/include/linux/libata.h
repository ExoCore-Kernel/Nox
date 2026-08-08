#pragma once

#include <stddef.h>
#include <stdarg.h>

#include <linux/ata.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/scatterlist.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>

#define ATA_TAG_POISON 0xfafbfcfdU

enum {
    ATA_MAX_QUEUE = 32,
    ATA_TAG_INTERNAL = ATA_MAX_QUEUE - 1,
    ATA_SHT_EMULATED = 1,
    ATA_SHT_CMD_PER_LUN = 1,
    ATA_SHT_THIS_ID = -1,

    ATA_DEV_UNKNOWN = 0,
    ATA_DEV_ATA = 1,
    ATA_DEV_ATA_UNSUP = 2,
    ATA_DEV_ATAPI = 3,
    ATA_DEV_ATAPI_UNSUP = 4,
    ATA_DEV_PMP = 5,
    ATA_DEV_NONE = 9,

    ATA_LFLAG_HRST_TO_RESUME = (1 << 0),
    ATA_LFLAG_SKIP_D2H_BSY = (1 << 1),
    ATA_LFLAG_NO_SRST = (1 << 2),
    ATA_LFLAG_ASSUME_ATA = (1 << 3),

    ATA_FLAG_SATA = (1 << 1),
    ATA_FLAG_NO_LEGACY = (1 << 2),
    ATA_FLAG_MMIO = (1 << 3),
    ATA_FLAG_PIO_DMA = (1 << 7),
    ATA_FLAG_NCQ = (1 << 10),
    ATA_FLAG_ACPI_SATA = (1 << 17),
    ATA_FLAG_AN = (1 << 18),
    ATA_FLAG_PMP = (1 << 19),
    ATA_FLAG_IPM = (1 << 20),
    ATA_FLAG_DISABLED = (1 << 23),

    ATA_PFLAG_FROZEN = (1 << 2),
    ATA_PFLAG_RESETTING = (1 << 8),

    ATA_QCFLAG_ACTIVE = (1 << 0),
    ATA_QCFLAG_SG = (1 << 1),
    ATA_QCFLAG_SINGLE = (1 << 2),
    ATA_QCFLAG_DMAMAP = ATA_QCFLAG_SG | ATA_QCFLAG_SINGLE,
    ATA_QCFLAG_FAILED = (1 << 16),

    AC_ERR_DEV = (1 << 0),
    AC_ERR_HSM = (1 << 1),
    AC_ERR_TIMEOUT = (1 << 2),
    AC_ERR_ATA_BUS = (1 << 4),
    AC_ERR_HOST_BUS = (1 << 5),

    ATA_EH_SOFTRESET = (1 << 1),
};

enum link_pm {
    NOT_AVAILABLE,
    MIN_POWER,
    MAX_PERFORMANCE,
    MEDIUM_POWER,
};

struct ata_port;
struct ata_link;
struct ata_queued_cmd;
struct ata_device;
struct ata_host;
struct pci_dev;

typedef int (*ata_prereset_fn_t)(struct ata_link *, unsigned long);
typedef int (*ata_reset_fn_t)(struct ata_link *, unsigned int *, unsigned long);
typedef void (*ata_postreset_fn_t)(struct ata_link *, unsigned int *);

struct ata_ioports {
    void __iomem *cmd_addr;
};

struct ata_eh_info {
    struct ata_device *dev;
    u32 serror;
    unsigned int err_mask;
    unsigned int action;
    unsigned int flags;
    char desc[80];
    int desc_len;
};

struct ata_eh_context {
    struct ata_eh_info i;
};

struct ata_device {
    struct ata_link *link;
    unsigned int devno;
    unsigned long flags;
    u64 n_sectors;
    unsigned int class;
    u16 id[ATA_ID_WORDS];
    unsigned int cdb_len;
};

struct ata_link {
    struct ata_port *ap;
    int pmp;
    unsigned int active_tag;
    u32 sactive;
    unsigned int flags;
    struct ata_eh_info eh_info;
    struct ata_eh_context eh_context;
    struct ata_device device[ATA_MAX_DEVICES];
};

struct ata_queued_cmd {
    struct ata_port *ap;
    struct ata_device *dev;
    struct scsi_cmnd *scsicmd;
    struct ata_taskfile tf;
    u8 cdb[ATAPI_CDB_LEN];
    unsigned long flags;
    unsigned int tag;
    unsigned int n_elem;
    unsigned int orig_n_elem;
    int dma_dir;
    unsigned int nbytes;
    struct scatterlist *cursg;
    struct scatterlist sgent;
    void *buf_virt;
    struct scatterlist *__sg;
    unsigned int err_mask;
    struct ata_taskfile result_tf;
    void *private_data;
};

struct ata_port_operations {
    void (*tf_read)(struct ata_port *, struct ata_taskfile *);
    u8 (*check_status)(struct ata_port *);
    u8 (*check_altstatus)(struct ata_port *);
    void (*dev_select)(struct ata_port *, unsigned int);
    int (*qc_defer)(struct ata_queued_cmd *);
    void (*qc_prep)(struct ata_queued_cmd *);
    unsigned int (*qc_issue)(struct ata_queued_cmd *);
    void (*pmp_attach)(struct ata_port *);
    void (*pmp_detach)(struct ata_port *);
    void (*freeze)(struct ata_port *);
    void (*thaw)(struct ata_port *);
    void (*error_handler)(struct ata_port *);
    void (*post_internal_cmd)(struct ata_queued_cmd *);
    void (*irq_clear)(struct ata_port *);
    int (*scr_read)(struct ata_port *, unsigned int, u32 *);
    int (*scr_write)(struct ata_port *, unsigned int, u32);
    int (*port_suspend)(struct ata_port *, int);
    int (*port_resume)(struct ata_port *);
    int (*enable_pm)(struct ata_port *, enum link_pm);
    void (*disable_pm)(struct ata_port *);
    int (*port_start)(struct ata_port *);
    void (*port_stop)(struct ata_port *);
    void (*host_stop)(struct ata_host *);
};

struct ata_port_info {
    struct scsi_host_template *sht;
    unsigned long flags;
    unsigned long link_flags;
    unsigned long pio_mask;
    unsigned long mwdma_mask;
    unsigned long udma_mask;
    const struct ata_port_operations *port_ops;
    irq_handler_t irq_handler;
    void *private_data;
};

struct ata_host {
    spinlock_t lock;
    struct device *dev;
    void __iomem **iomap;
    unsigned int n_ports;
    void *private_data;
    const struct ata_port_operations *ops;
    unsigned long flags;
    struct ata_port **ports;
};

struct ata_port {
    struct Scsi_Host *scsi_host;
    const struct ata_port_operations *ops;
    spinlock_t *lock;
    unsigned long flags;
    unsigned int pflags;
    unsigned int print_id;
    unsigned int port_no;
    struct ata_ioports ioaddr;
    unsigned int pio_mask;
    unsigned int mwdma_mask;
    unsigned int udma_mask;
    unsigned int cbl;
    struct ata_queued_cmd qcmd[ATA_MAX_QUEUE];
    unsigned int qc_active;
    struct ata_link link;
    int nr_pmp_links;
    struct ata_host *host;
    struct device *dev;
    u32 msg_enable;
    enum link_pm pm_policy;
    void *private_data;
};

extern struct class_device_attribute class_device_attr_link_power_management_policy;
extern const struct ata_port_operations ata_dummy_port_ops;
extern const struct ata_port_info ata_dummy_port_info;

#define ata_for_each_sg(sg, qc) \
    for (unsigned int __i = 0; __i < (qc)->n_elem && (((sg) = &(qc)->__sg[__i]) != 0); ++__i)

#define ata_port_for_each_link(link, ap) \
    for ((link) = &(ap)->link; (link) != 0; (link) = 0)

static inline int ata_link_active(const struct ata_link *link) {
    return link != 0 && (link->sactive != 0 || link->active_tag < ATA_MAX_QUEUE);
}

static inline struct ata_queued_cmd *ata_qc_from_tag(struct ata_port *ap, unsigned int tag) {
    if (ap == 0 || tag >= ATA_MAX_QUEUE) return 0;
    return &ap->qcmd[tag];
}

static inline int ata_tag_internal(unsigned int tag) { return tag == ATA_TAG_INTERNAL; }
static inline int ata_is_ncq(u8 protocol) { return protocol == ATA_PROT_NCQ; }
static inline int ata_port_is_dummy(const struct ata_port *ap) {
    return ap == 0 || ap->ops == &ata_dummy_port_ops;
}
static inline int ata_ratelimit(void) { return 1; }

#define ata_port_printk(ap, level, format, ...) \
    printk(level "ata%u: " format, (ap) ? (ap)->print_id : 0u, ##__VA_ARGS__)
#define ata_link_printk(link, level, format, ...) \
    ata_port_printk((link) ? (link)->ap : 0, level, format, ##__VA_ARGS__)

struct ata_host *ata_host_alloc_pinfo(struct device *dev,
                                      const struct ata_port_info * const *ppi,
                                      int n_ports);
int ata_host_activate(struct ata_host *host, int irq,
                      irq_handler_t irq_handler, unsigned long irq_flags,
                      struct scsi_host_template *sht);
void ata_host_detach(struct ata_host *host);
void ata_pci_remove_one(struct pci_dev *pdev);

int ata_pad_alloc(struct ata_port *ap, struct device *dev);
void ata_port_pbar_desc(struct ata_port *ap, int bar, int offset, const char *name);

u32 ata_wait_register(void __iomem *reg, u32 mask, u32 val,
                      unsigned long interval_msec, unsigned long timeout_msec);
void ata_wait_after_reset(struct ata_port *ap, unsigned long deadline);
int ata_wait_ready(struct ata_port *ap, unsigned long deadline);

void ata_tf_init(struct ata_device *dev, struct ata_taskfile *tf);
void ata_tf_to_fis(const struct ata_taskfile *tf, u8 pmp, int is_cmd, u8 *fis);
void ata_tf_from_fis(const u8 *fis, struct ata_taskfile *tf);
unsigned int ata_dev_classify(const struct ata_taskfile *tf);

void ata_noop_dev_select(struct ata_port *ap, unsigned int device);
int sata_pmp_qc_defer_cmd_switch(struct ata_queued_cmd *qc);
int ata_qc_complete_multiple(struct ata_port *ap, u32 qc_active, void *finish_qc);
void ata_port_abort(struct ata_port *ap);
void ata_port_freeze(struct ata_port *ap);
void ata_ehi_clear_desc(struct ata_eh_info *ehi);
void ata_ehi_push_desc(struct ata_eh_info *ehi, const char *fmt, ...);
void ata_ehi_hotplugged(struct ata_eh_info *ehi);
void sata_async_notification(struct ata_port *ap);

int ata_link_offline(struct ata_link *link);
int ata_link_online(struct ata_link *link);
int ata_std_prereset(struct ata_link *link, unsigned long deadline);
int sata_std_hardreset(struct ata_link *link, unsigned int *class, unsigned long deadline);
int sata_link_hardreset(struct ata_link *link, const unsigned long *timing, unsigned long deadline);
const unsigned long *sata_ehc_deb_timing(struct ata_eh_context *ehc);
void ata_std_postreset(struct ata_link *link, unsigned int *classes);
void sata_pmp_error_handler(struct ata_port *ap);
int sata_pmp_std_prereset(struct ata_link *link, unsigned long deadline);
int sata_pmp_std_hardreset(struct ata_link *link, unsigned int *class, unsigned long deadline);
void sata_pmp_std_postreset(struct ata_link *link, unsigned int *classes);
void ata_std_error_handler(struct ata_port *ap);
void ata_do_eh(struct ata_port *ap, ata_prereset_fn_t prereset,
               ata_reset_fn_t softreset, ata_reset_fn_t hardreset,
               ata_postreset_fn_t postreset);
void sata_pmp_do_eh(struct ata_port *ap,
                    ata_prereset_fn_t prereset, ata_reset_fn_t softreset,
                    ata_reset_fn_t hardreset, ata_postreset_fn_t postreset,
                    ata_prereset_fn_t pmp_prereset, ata_reset_fn_t pmp_softreset,
                    ata_reset_fn_t pmp_hardreset, ata_postreset_fn_t pmp_postreset);

int ata_scsi_ioctl(struct scsi_device *dev, int cmd, void *arg);
int ata_scsi_queuecmd(struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *));
int ata_scsi_change_queue_depth(struct scsi_device *sdev, int queue_depth);
int ata_scsi_slave_config(struct scsi_device *sdev);
void ata_scsi_slave_destroy(struct scsi_device *sdev);
int ata_std_bios_param(struct scsi_device *sdev, struct block_device *bdev,
                       sector_t capacity, int *geom);
