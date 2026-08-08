#pragma once

#include <linux/types.h>
#include <linux/dma-mapping.h>

struct scatterlist {
    void *address;
    unsigned int length;
    dma_addr_t dma_address;
    unsigned int dma_length;
};

#define sg_dma_address(sg) ((sg)->dma_address)
#define sg_dma_len(sg) ((sg)->dma_length)

static inline void sg_init_one(struct scatterlist *sg, void *buffer, unsigned int length) {
    if (sg == 0) return;
    sg->address = buffer;
    sg->length = length;
    sg->dma_address = DMA_MAPPING_ERROR;
    sg->dma_length = 0;
}
