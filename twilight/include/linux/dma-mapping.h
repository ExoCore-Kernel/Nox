#pragma once

#include <stddef.h>

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/types.h>

#define DMA_BIT_MASK(n) ((n) == 64 ? ~0ull : ((1ull << (n)) - 1ull))
#define DMA_MAPPING_ERROR (~(dma_addr_t)0)

enum dma_data_direction {
    DMA_BIDIRECTIONAL = 0,
    DMA_TO_DEVICE = 1,
    DMA_FROM_DEVICE = 2,
    DMA_NONE = 3,
};

void *dma_alloc_coherent(struct device *dev,
                         size_t size,
                         dma_addr_t *dma_handle,
                         gfp_t flags);
void dma_free_coherent(struct device *dev,
                       size_t size,
                       void *cpu_addr,
                       dma_addr_t dma_handle);

dma_addr_t dma_map_single(struct device *dev,
                          void *cpu_addr,
                          size_t size,
                          enum dma_data_direction direction);
void dma_unmap_single(struct device *dev,
                      dma_addr_t dma_addr,
                      size_t size,
                      enum dma_data_direction direction);

int dma_mapping_error(struct device *dev, dma_addr_t dma_addr);
void dma_sync_single_for_cpu(struct device *dev,
                             dma_addr_t dma_addr,
                             size_t size,
                             enum dma_data_direction direction);
void dma_sync_single_for_device(struct device *dev,
                                dma_addr_t dma_addr,
                                size_t size,
                                enum dma_data_direction direction);

int dma_set_mask(struct device *dev, u64 mask);
int dma_set_coherent_mask(struct device *dev, u64 mask);
int dma_set_mask_and_coherent(struct device *dev, u64 mask);
u64 dma_get_mask(struct device *dev);

static inline void *dma_alloc_wc(struct device *dev,
                                 size_t size,
                                 dma_addr_t *dma_handle,
                                 gfp_t flags) {
    return dma_alloc_coherent(dev, size, dma_handle, flags);
}

static inline void dma_free_wc(struct device *dev,
                               size_t size,
                               void *cpu_addr,
                               dma_addr_t dma_handle) {
    dma_free_coherent(dev, size, cpu_addr, dma_handle);
}
