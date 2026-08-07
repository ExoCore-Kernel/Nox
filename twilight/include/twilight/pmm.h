#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

#define TWILIGHT_PAGE_SIZE 4096ull

struct pmm_stats {
    uint64_t reported_bytes;
    uint64_t usable_bytes;
    uint64_t free_bytes;
    uint64_t used_bytes;
    uint64_t highest_physical_address;
    uint64_t addressable_pages;
    uint64_t usable_pages;
    uint64_t free_pages;
    uint64_t pinned_pages;
    uint64_t metadata_pages;
};

bool pmm_init(const struct limine_memmap_response *memory_map, uint64_t hhdm_offset);
bool pmm_is_initialized(void);

uint64_t pmm_alloc_page(void);
uint64_t pmm_alloc_pages(size_t count);
uint64_t pmm_alloc_pages_ex(size_t count, size_t alignment_pages, uint64_t max_physical_address);

bool pmm_free_page(uint64_t physical_address);
bool pmm_free_pages(uint64_t physical_address, size_t count);
bool pmm_reserve_range(uint64_t physical_address, uint64_t length);

void *pmm_phys_to_virt(uint64_t physical_address);
uint64_t pmm_hhdm_offset(void);

void pmm_get_stats(struct pmm_stats *out);
bool pmm_self_test(void);
