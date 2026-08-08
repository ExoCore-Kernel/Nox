#pragma once

#include <linux/blkdev.h>
#include <linux/types.h>

struct scsi_cmnd;
struct scsi_device { unsigned int reserved; };
struct class_device_attribute { const char *name; };
struct Scsi_Host { void *hostdata; };

struct scsi_host_template {
    void *module;
    const char *name;
    int (*ioctl)(struct scsi_device *, int, void *);
    int (*queuecommand)(struct scsi_cmnd *, void (*)(struct scsi_cmnd *));
    int (*change_queue_depth)(struct scsi_device *, int);
    int can_queue;
    int this_id;
    int sg_tablesize;
    int cmd_per_lun;
    int emulated;
    int use_clustering;
    const char *proc_name;
    unsigned long dma_boundary;
    int (*slave_configure)(struct scsi_device *);
    void (*slave_destroy)(struct scsi_device *);
    int (*bios_param)(struct scsi_device *, struct block_device *, sector_t, int *);
    struct class_device_attribute **shost_attrs;
};
