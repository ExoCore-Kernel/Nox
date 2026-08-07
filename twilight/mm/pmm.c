#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <limine.h>
#include <twilight/pmm.h>

#define PMM_BITMAP_COUNT 3ull

static uint8_t *used_bitmap;
static uint8_t *usable_bitmap;
static uint8_t *pinned_bitmap;
static uint64_t bitmap_bytes;
static uint64_t page_count;
static uint64_t free_pages_count;
static uint64_t usable_pages_count;
static uint64_t pinned_pages_count;
static uint64_t metadata_pages_count;
static uint64_t reported_bytes_count;
static uint64_t usable_bytes_count;
static uint64_t highest_physical_address;
static uint64_t managed_physical_limit;
static uint64_t direct_map_offset;
static uint64_t allocation_hint;
static uint64_t metadata_phys_start;
static uint64_t metadata_phys_end;
static bool initialized;

static uint64_t align_down(uint64_t value, uint64_t alignment) {
    return value & ~(alignment - 1ull);
}

static bool align_up_checked(uint64_t value, uint64_t alignment, uint64_t *out) {
    const uint64_t mask = alignment - 1ull;
    if (value > UINT64_MAX - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

static uint64_t saturating_add(uint64_t a, uint64_t b) {
    if (a > UINT64_MAX - b) return UINT64_MAX;
    return a + b;
}

static bool bit_get(const uint8_t *bitmap, uint64_t bit) {
    return (bitmap[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0;
}

static void bit_set(uint8_t *bitmap, uint64_t bit, bool value) {
    const uint8_t mask = (uint8_t)(1u << (bit & 7u));
    uint8_t *byte = &bitmap[bit >> 3];
    if (value) *byte |= mask;
    else *byte &= (uint8_t)~mask;
}

static bool page_is_allocatable(uint64_t page) {
    if (page >= page_count) return false;
    return bit_get(usable_bitmap, page) && !bit_get(used_bitmap, page) && !bit_get(pinned_bitmap, page);
}

static void mark_page_used(uint64_t page, bool used) {
    if (page >= page_count || !bit_get(usable_bitmap, page)) return;

    const bool old = bit_get(used_bitmap, page);
    if (old == used) return;

    bit_set(used_bitmap, page, used);
    if (used) {
        if (free_pages_count != 0) --free_pages_count;
    } else {
        ++free_pages_count;
    }
}

static bool metadata_range_contains(uint64_t physical_address) {
    return physical_address >= metadata_phys_start && physical_address < metadata_phys_end;
}

static bool range_fits(uint64_t first_page, uint64_t count, uint64_t max_physical_address) {
    if (count == 0 || first_page >= page_count) return false;
    if (count > page_count - first_page) return false;

    const uint64_t last_page = first_page + count - 1ull;
    if (last_page > UINT64_MAX / TWILIGHT_PAGE_SIZE) return false;
    const uint64_t last_base = last_page * TWILIGHT_PAGE_SIZE;

    if (max_physical_address != 0) {
        if (last_base > UINT64_MAX - (TWILIGHT_PAGE_SIZE - 1ull)) return false;
        if (last_base + (TWILIGHT_PAGE_SIZE - 1ull) > max_physical_address) return false;
    }

    for (uint64_t page = first_page; page <= last_page; ++page) {
        if (!page_is_allocatable(page)) return false;
    }
    return true;
}

static uint64_t find_run(uint64_t start_page,
                         uint64_t count,
                         uint64_t alignment_pages,
                         uint64_t max_physical_address) {
    if (count == 0 || count > page_count) return UINT64_MAX;
    if (alignment_pages == 0) alignment_pages = 1;

    for (unsigned pass = 0; pass < 2; ++pass) {
        uint64_t page = pass == 0 ? start_page : 0;
        const uint64_t end = pass == 0 ? page_count : start_page;

        while (page < end) {
            const uint64_t remainder = page % alignment_pages;
            if (remainder != 0) {
                const uint64_t bump = alignment_pages - remainder;
                if (page > UINT64_MAX - bump) break;
                page += bump;
                continue;
            }

            if (count > end - page) break;
            if (range_fits(page, count, max_physical_address)) return page;
            ++page;
        }
    }

    return UINT64_MAX;
}

bool pmm_init(const struct limine_memmap_response *memory_map, uint64_t hhdm_offset) {
    initialized = false;
    used_bitmap = 0;
    usable_bitmap = 0;
    pinned_bitmap = 0;

    if (memory_map == 0 || memory_map->entries == 0 || memory_map->entry_count == 0) return false;

    reported_bytes_count = 0;
    usable_bytes_count = 0;
    highest_physical_address = 0;
    managed_physical_limit = 0;

    /*
     * Keep diagnostics for the complete Limine map, but size allocator metadata
     * only from the highest page Twilight can actually allocate. Firmware maps
     * often contain very high MMIO/reserved ranges; covering those holes with a
     * dense bitmap wasted tens of MiB on otherwise tiny machines.
     */
    for (uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry == 0 || entry->length == 0) continue;

        reported_bytes_count = saturating_add(reported_bytes_count, entry->length);
        const uint64_t end = saturating_add(entry->base, entry->length);
        if (end > highest_physical_address) highest_physical_address = end;

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_bytes_count = saturating_add(usable_bytes_count, entry->length);
            if (end > managed_physical_limit) managed_physical_limit = end;
        }
    }

    if (managed_physical_limit == 0) return false;

    page_count = managed_physical_limit / TWILIGHT_PAGE_SIZE;
    if ((managed_physical_limit % TWILIGHT_PAGE_SIZE) != 0) ++page_count;
    if (page_count == 0) return false;

    bitmap_bytes = page_count / 8ull;
    if ((page_count & 7ull) != 0) ++bitmap_bytes;
    if (bitmap_bytes == 0 || bitmap_bytes > UINT64_MAX / PMM_BITMAP_COUNT) return false;

    const uint64_t raw_metadata_bytes = bitmap_bytes * PMM_BITMAP_COUNT;
    uint64_t metadata_bytes_aligned = 0;
    if (!align_up_checked(raw_metadata_bytes, TWILIGHT_PAGE_SIZE, &metadata_bytes_aligned)) return false;
    metadata_pages_count = metadata_bytes_aligned / TWILIGHT_PAGE_SIZE;
    if (metadata_pages_count == 0) return false;

    bool metadata_found = false;
    metadata_phys_start = 0;
    metadata_phys_end = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry == 0 || entry->type != LIMINE_MEMMAP_USABLE || entry->length == 0) continue;

        uint64_t start = 0;
        if (!align_up_checked(entry->base, TWILIGHT_PAGE_SIZE, &start)) continue;
        const uint64_t end_raw = saturating_add(entry->base, entry->length);
        const uint64_t end = align_down(end_raw, TWILIGHT_PAGE_SIZE);
        if (end <= start || end - start < metadata_bytes_aligned) continue;

        metadata_phys_start = start;
        metadata_phys_end = start + metadata_bytes_aligned;
        metadata_found = true;
        break;
    }

    if (!metadata_found) return false;
    if (metadata_phys_start > UINT64_MAX - hhdm_offset) return false;

    direct_map_offset = hhdm_offset;
    used_bitmap = (uint8_t *)(uintptr_t)(direct_map_offset + metadata_phys_start);
    usable_bitmap = used_bitmap + bitmap_bytes;
    pinned_bitmap = usable_bitmap + bitmap_bytes;

    for (uint64_t i = 0; i < bitmap_bytes; ++i) {
        used_bitmap[i] = 0xffu;
        usable_bitmap[i] = 0x00u;
        pinned_bitmap[i] = 0x00u;
    }

    usable_pages_count = 0;

    for (uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const struct limine_memmap_entry *entry = memory_map->entries[i];
        if (entry == 0 || entry->type != LIMINE_MEMMAP_USABLE || entry->length == 0) continue;

        uint64_t start = 0;
        if (!align_up_checked(entry->base, TWILIGHT_PAGE_SIZE, &start)) continue;
        const uint64_t end = align_down(saturating_add(entry->base, entry->length), TWILIGHT_PAGE_SIZE);
        if (end <= start) continue;

        uint64_t page = start / TWILIGHT_PAGE_SIZE;
        const uint64_t end_page = end / TWILIGHT_PAGE_SIZE;
        while (page < end_page && page < page_count) {
            if (!bit_get(usable_bitmap, page)) {
                bit_set(usable_bitmap, page, true);
                bit_set(used_bitmap, page, false);
                ++usable_pages_count;
            }
            ++page;
        }
    }

    free_pages_count = usable_pages_count;
    pinned_pages_count = 0;

    const uint64_t metadata_first_page = metadata_phys_start / TWILIGHT_PAGE_SIZE;
    for (uint64_t i = 0; i < metadata_pages_count; ++i) {
        const uint64_t page = metadata_first_page + i;
        if (page >= page_count || !bit_get(usable_bitmap, page)) return false;
        if (!bit_get(pinned_bitmap, page)) {
            bit_set(pinned_bitmap, page, true);
            ++pinned_pages_count;
        }
        mark_page_used(page, true);
    }

    allocation_hint = metadata_first_page + metadata_pages_count;
    if (allocation_hint >= page_count) allocation_hint = 0;

    initialized = true;
    return true;
}

bool pmm_is_initialized(void) {
    return initialized;
}

uint64_t pmm_alloc_pages_ex(size_t count, size_t alignment_pages, uint64_t max_physical_address) {
    if (!initialized || count == 0) return 0;

    const uint64_t count64 = (uint64_t)count;
    const uint64_t alignment64 = alignment_pages == 0 ? 1ull : (uint64_t)alignment_pages;
    if (count64 > free_pages_count) return 0;

    const uint64_t first = find_run(allocation_hint, count64, alignment64, max_physical_address);
    if (first == UINT64_MAX) return 0;

    for (uint64_t i = 0; i < count64; ++i) mark_page_used(first + i, true);

    allocation_hint = first + count64;
    if (allocation_hint >= page_count) allocation_hint = 0;

    return first * TWILIGHT_PAGE_SIZE;
}

uint64_t pmm_alloc_pages(size_t count) {
    return pmm_alloc_pages_ex(count, 1, 0);
}

uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages_ex(1, 1, 0);
}

bool pmm_free_pages(uint64_t physical_address, size_t count) {
    if (!initialized || count == 0) return false;
    if ((physical_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;

    const uint64_t first = physical_address / TWILIGHT_PAGE_SIZE;
    const uint64_t count64 = (uint64_t)count;
    if (first >= page_count || count64 > page_count - first) return false;

    for (uint64_t i = 0; i < count64; ++i) {
        const uint64_t page = first + i;
        const uint64_t address = page * TWILIGHT_PAGE_SIZE;
        if (!bit_get(usable_bitmap, page)) return false;
        if (bit_get(pinned_bitmap, page)) return false;
        if (metadata_range_contains(address)) return false;
        if (!bit_get(used_bitmap, page)) return false;
    }

    for (uint64_t i = 0; i < count64; ++i) mark_page_used(first + i, false);
    if (first < allocation_hint) allocation_hint = first;
    return true;
}

bool pmm_free_page(uint64_t physical_address) {
    return pmm_free_pages(physical_address, 1);
}

bool pmm_reserve_range(uint64_t physical_address, uint64_t length) {
    if (!initialized || length == 0) return false;

    const uint64_t start = align_down(physical_address, TWILIGHT_PAGE_SIZE);
    const uint64_t raw_end = saturating_add(physical_address, length);
    uint64_t end = 0;
    if (!align_up_checked(raw_end, TWILIGHT_PAGE_SIZE, &end)) end = UINT64_MAX & ~(TWILIGHT_PAGE_SIZE - 1ull);

    uint64_t page = start / TWILIGHT_PAGE_SIZE;
    const uint64_t end_page = end / TWILIGHT_PAGE_SIZE;
    while (page < end_page && page < page_count) {
        if (bit_get(usable_bitmap, page)) {
            if (!bit_get(pinned_bitmap, page)) {
                bit_set(pinned_bitmap, page, true);
                ++pinned_pages_count;
            }
            mark_page_used(page, true);
        }
        ++page;
    }
    return true;
}

void *pmm_phys_to_virt(uint64_t physical_address) {
    if (!initialized || physical_address > UINT64_MAX - direct_map_offset) return 0;
    return (void *)(uintptr_t)(direct_map_offset + physical_address);
}

uint64_t pmm_hhdm_offset(void) {
    return initialized ? direct_map_offset : 0;
}

void pmm_get_stats(struct pmm_stats *out) {
    if (out == 0) return;

    out->reported_bytes = reported_bytes_count;
    out->usable_bytes = usable_bytes_count;
    out->free_bytes = free_pages_count * TWILIGHT_PAGE_SIZE;
    out->used_bytes = (usable_pages_count - free_pages_count) * TWILIGHT_PAGE_SIZE;
    out->highest_physical_address = highest_physical_address;
    out->addressable_pages = page_count;
    out->usable_pages = usable_pages_count;
    out->free_pages = free_pages_count;
    out->pinned_pages = pinned_pages_count;
    out->metadata_pages = metadata_pages_count;
}

bool pmm_self_test(void) {
    if (!initialized) return false;

    const uint64_t before = free_pages_count;
    const uint64_t a = pmm_alloc_page();
    const uint64_t b = pmm_alloc_pages_ex(4, 4, 0xffffffffull);

    if (a == 0 || b == 0 || a == b) {
        if (a != 0) (void)pmm_free_page(a);
        if (b != 0) (void)pmm_free_pages(b, 4);
        return false;
    }

    volatile uint64_t *va = (volatile uint64_t *)pmm_phys_to_virt(a);
    volatile uint64_t *vb0 = (volatile uint64_t *)pmm_phys_to_virt(b);
    volatile uint64_t *vb3 = (volatile uint64_t *)pmm_phys_to_virt(b + 3ull * TWILIGHT_PAGE_SIZE);
    if (va == 0 || vb0 == 0 || vb3 == 0) return false;

    *va = 0x5457494c49474854ull;
    *vb0 = 0x504d4d5445535430ull;
    *vb3 = 0x504d4d5445535433ull;

    if (*va != 0x5457494c49474854ull ||
        *vb0 != 0x504d4d5445535430ull ||
        *vb3 != 0x504d4d5445535433ull) {
        return false;
    }

    if (!pmm_free_page(a)) return false;
    if (!pmm_free_pages(b, 4)) return false;

    /* A second free must be rejected rather than corrupting allocator state. */
    if (pmm_free_page(a)) return false;

    return free_pages_count == before;
}
