#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/dma-mapping.h>
#include <twilight/pmm.h>
#include <twilight/vmm.h>

#define DMA_TRACKED_ALLOCATIONS 128u

struct dma_allocation {
    bool used;
    uint64_t physical;
    void *virtual_address;
    size_t page_count;
};

static struct dma_allocation allocations[DMA_TRACKED_ALLOCATIONS];
static volatile uint32_t dma_lock_word;

static void zero_bytes(void *pointer, size_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&dma_lock_word, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&dma_lock_word, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

static u64 effective_mask(struct device *dev, bool coherent) {
    if (dev == 0) return DMA_BIT_MASK(32);
    if (coherent && dev->coherent_dma_mask != 0) return dev->coherent_dma_mask;
    if (dev->dma_mask != 0 && *dev->dma_mask != 0) return *dev->dma_mask;
    return DMA_BIT_MASK(32);
}

u64 dma_get_mask(struct device *dev) {
    return effective_mask(dev, false);
}

int dma_set_mask(struct device *dev, u64 mask) {
    if (dev == 0 || mask == 0) return -EINVAL;
    dev->dma_mask_storage = mask;
    dev->dma_mask = &dev->dma_mask_storage;
    return 0;
}

int dma_set_coherent_mask(struct device *dev, u64 mask) {
    if (dev == 0 || mask == 0) return -EINVAL;
    dev->coherent_dma_mask = mask;
    return 0;
}

int dma_set_mask_and_coherent(struct device *dev, u64 mask) {
    const int streaming = dma_set_mask(dev, mask);
    if (streaming != 0) return streaming;
    return dma_set_coherent_mask(dev, mask);
}

void *dma_alloc_coherent(struct device *dev,
                         size_t size,
                         dma_addr_t *dma_handle,
                         gfp_t flags) {
    (void)flags;
    if (size == 0 || dma_handle == 0 || !pmm_is_initialized()) return 0;

    const size_t pages = (size + TWILIGHT_PAGE_SIZE - 1u) / TWILIGHT_PAGE_SIZE;
    if (pages == 0) return 0;

    const u64 mask = effective_mask(dev, true);
    const uint64_t physical = pmm_alloc_pages_ex(pages, 1u, mask);
    if (physical == 0) return 0;

    void *virtual_address = pmm_phys_to_virt(physical);
    if (virtual_address == 0) {
        (void)pmm_free_pages(physical, pages);
        return 0;
    }

    const uint64_t saved_flags = lock_irqsave();
    struct dma_allocation *slot = 0;
    for (size_t i = 0; i < DMA_TRACKED_ALLOCATIONS; ++i) {
        if (!allocations[i].used) {
            slot = &allocations[i];
            break;
        }
    }
    if (slot == 0) {
        unlock_irqrestore(saved_flags);
        (void)pmm_free_pages(physical, pages);
        return 0;
    }

    *slot = (struct dma_allocation){
        .used = true,
        .physical = physical,
        .virtual_address = virtual_address,
        .page_count = pages,
    };
    unlock_irqrestore(saved_flags);

    zero_bytes(virtual_address, pages * (size_t)TWILIGHT_PAGE_SIZE);
    *dma_handle = (dma_addr_t)physical;
    return virtual_address;
}

void dma_free_coherent(struct device *dev,
                       size_t size,
                       void *cpu_addr,
                       dma_addr_t dma_handle) {
    (void)dev;
    (void)size;
    if (cpu_addr == 0 || dma_handle == DMA_MAPPING_ERROR) return;

    const uint64_t saved_flags = lock_irqsave();
    for (size_t i = 0; i < DMA_TRACKED_ALLOCATIONS; ++i) {
        struct dma_allocation *slot = &allocations[i];
        if (!slot->used || slot->virtual_address != cpu_addr ||
            slot->physical != (uint64_t)dma_handle) {
            continue;
        }

        const uint64_t physical = slot->physical;
        const size_t pages = slot->page_count;
        *slot = (struct dma_allocation){0};
        unlock_irqrestore(saved_flags);
        (void)pmm_free_pages(physical, pages);
        return;
    }
    unlock_irqrestore(saved_flags);
}

static bool contiguous_mapping(void *cpu_addr,
                               size_t size,
                               uint64_t *physical_out) {
    if (cpu_addr == 0 || size == 0 || physical_out == 0) return false;

    const uint64_t address = (uint64_t)(uintptr_t)cpu_addr;
    uint64_t first_physical = 0;
    if (!vmm_translate(vmm_kernel_space(), address, &first_physical, 0)) return false;

    const uint64_t first_page_physical = first_physical & ~(TWILIGHT_PAGE_SIZE - 1ull);
    const uint64_t first_page_virtual = address & ~(TWILIGHT_PAGE_SIZE - 1ull);
    const size_t offset = (size_t)(address - first_page_virtual);
    const size_t pages = (offset + size + TWILIGHT_PAGE_SIZE - 1u) / TWILIGHT_PAGE_SIZE;

    for (size_t i = 1; i < pages; ++i) {
        uint64_t physical = 0;
        const uint64_t virtual_page = first_page_virtual + (uint64_t)i * TWILIGHT_PAGE_SIZE;
        if (!vmm_translate(vmm_kernel_space(), virtual_page, &physical, 0)) return false;
        if ((physical & ~(TWILIGHT_PAGE_SIZE - 1ull)) !=
            first_page_physical + (uint64_t)i * TWILIGHT_PAGE_SIZE) {
            return false;
        }
    }

    *physical_out = first_physical;
    return true;
}

dma_addr_t dma_map_single(struct device *dev,
                          void *cpu_addr,
                          size_t size,
                          enum dma_data_direction direction) {
    if (direction == DMA_NONE) return DMA_MAPPING_ERROR;

    uint64_t physical = 0;
    if (!contiguous_mapping(cpu_addr, size, &physical)) return DMA_MAPPING_ERROR;

    const u64 mask = effective_mask(dev, false);
    if (size - 1u > UINT64_MAX - physical) return DMA_MAPPING_ERROR;
    if (physical + (uint64_t)size - 1ull > mask) return DMA_MAPPING_ERROR;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return (dma_addr_t)physical;
}

void dma_unmap_single(struct device *dev,
                      dma_addr_t dma_addr,
                      size_t size,
                      enum dma_data_direction direction) {
    (void)dev;
    (void)dma_addr;
    (void)size;
    (void)direction;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

int dma_mapping_error(struct device *dev, dma_addr_t dma_addr) {
    (void)dev;
    return dma_addr == DMA_MAPPING_ERROR;
}

void dma_sync_single_for_cpu(struct device *dev,
                             dma_addr_t dma_addr,
                             size_t size,
                             enum dma_data_direction direction) {
    (void)dev; (void)dma_addr; (void)size; (void)direction;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

void dma_sync_single_for_device(struct device *dev,
                                dma_addr_t dma_addr,
                                size_t size,
                                enum dma_data_direction direction) {
    (void)dev; (void)dma_addr; (void)size; (void)direction;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}
