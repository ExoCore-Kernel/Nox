#pragma once

#include <stdbool.h>
#include <linux/types.h>

#define ATA_DMA_BOUNDARY 0xffffUL
#define ATA_DMA_MASK     0xffffffffULL

enum {
    ATA_MAX_DEVICES = 2,
    ATA_MAX_PRD = 256,
    ATA_SECT_SIZE = 512,
    ATA_ID_WORDS = 256,
    ATAPI_CDB_LEN = 16,

    ATA_UDMA0 = (1 << 0),
    ATA_UDMA1 = ATA_UDMA0 | (1 << 1),
    ATA_UDMA2 = ATA_UDMA1 | (1 << 2),
    ATA_UDMA3 = ATA_UDMA2 | (1 << 3),
    ATA_UDMA4 = ATA_UDMA3 | (1 << 4),
    ATA_UDMA5 = ATA_UDMA4 | (1 << 5),
    ATA_UDMA6 = ATA_UDMA5 | (1 << 6),

    ATA_BUSY = (1 << 7),
    ATA_DRDY = (1 << 6),
    ATA_DF   = (1 << 5),
    ATA_DRQ  = (1 << 3),
    ATA_ERR  = (1 << 0),
    ATA_SRST = (1 << 2),
    ATA_DEVICE_OBS = (1 << 7) | (1 << 5),

    ATA_CMD_ID_ATA = 0xec,
    ATA_CMD_ID_ATAPI = 0xa1,
    ATA_CMD_READ = 0xc8,
    ATA_CMD_READ_EXT = 0x25,
    ATA_CMD_WRITE = 0xca,
    ATA_CMD_WRITE_EXT = 0x35,
    ATA_CMD_FPDMA_READ = 0x60,
    ATA_CMD_FPDMA_WRITE = 0x61,

    SATA_PMP_MAX_PORTS = 15,
    SATA_PMP_CTRL_PORT = 15,
    SATA_PMP_GSCR_DWORDS = 128,

    ATA_CBL_SATA = 5,

    SCR_STATUS = 0,
    SCR_ERROR = 1,
    SCR_CONTROL = 2,
    SCR_ACTIVE = 3,
    SCR_NOTIFICATION = 4,

    SERR_DATA_RECOVERED = (1 << 0),
    SERR_COMM_RECOVERED = (1 << 1),
    SERR_DATA = (1 << 8),
    SERR_PERSISTENT = (1 << 9),
    SERR_PROTOCOL = (1 << 10),
    SERR_INTERNAL = (1 << 11),
    SERR_PHYRDY_CHG = (1 << 16),
    SERR_PHY_INT_ERR = (1 << 17),
    SERR_COMM_WAKE = (1 << 18),
    SERR_10B_8B_ERR = (1 << 19),
    SERR_DISPARITY = (1 << 20),
    SERR_CRC = (1 << 21),
    SERR_HANDSHAKE = (1 << 22),
    SERR_LINK_SEQ_ERR = (1 << 23),
    SERR_TRANS_ST_ERROR = (1 << 24),
    SERR_UNRECOG_FIS = (1 << 25),
    SERR_DEV_XCHG = (1 << 26),

    ATA_TFLAG_LBA48 = (1 << 0),
    ATA_TFLAG_ISADDR = (1 << 1),
    ATA_TFLAG_DEVICE = (1 << 2),
    ATA_TFLAG_WRITE = (1 << 3),
    ATA_TFLAG_LBA = (1 << 4),
    ATA_TFLAG_FUA = (1 << 5),
    ATA_TFLAG_POLLING = (1 << 6),
};

enum ata_tf_protocols {
    ATA_PROT_UNKNOWN,
    ATA_PROT_NODATA,
    ATA_PROT_PIO,
    ATA_PROT_DMA,
    ATA_PROT_NCQ,
    ATA_PROT_ATAPI,
    ATA_PROT_ATAPI_NODATA,
    ATA_PROT_ATAPI_DMA,
};

struct ata_prd {
    u32 addr;
    u32 flags_len;
};

struct ata_taskfile {
    unsigned long flags;
    u8 protocol;
    u8 ctl;
    u8 hob_feature;
    u8 hob_nsect;
    u8 hob_lbal;
    u8 hob_lbam;
    u8 hob_lbah;
    u8 feature;
    u8 nsect;
    u8 lbal;
    u8 lbam;
    u8 lbah;
    u8 device;
    u8 command;
};

static inline bool is_atapi_taskfile(const struct ata_taskfile *tf) {
    if (tf == 0) return false;
    return tf->protocol == ATA_PROT_ATAPI ||
           tf->protocol == ATA_PROT_ATAPI_NODATA ||
           tf->protocol == ATA_PROT_ATAPI_DMA;
}
