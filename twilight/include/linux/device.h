#pragma once

#include <linux/types.h>

struct device {
    void *driver_data;
    const char *init_name;

    /* Linux-compatible DMA mask shape. dma_mask points at the storage below
     * unless a bus-specific wrapper replaces it. */
    u64 dma_mask_storage;
    u64 *dma_mask;
    u64 coherent_dma_mask;
};

static inline void dev_set_drvdata(struct device *dev, void *data) {
    if (dev != 0) dev->driver_data = data;
}

static inline void *dev_get_drvdata(const struct device *dev) {
    return dev != 0 ? dev->driver_data : 0;
}

static inline const char *dev_name(const struct device *dev) {
    return (dev != 0 && dev->init_name != 0) ? dev->init_name : "device";
}
