#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/mmio.h>
#include <twilight/pmm.h>
#include <twilight/vmm.h>

#define MMIO_ARENA_BYTES (1ull << 30)
#define MMIO_ARENA_PAGES (MMIO_ARENA_BYTES / TWILIGHT_PAGE_SIZE)
#define MMIO_BITMAP_BYTES ((MMIO_ARENA_PAGES + 7ull) / 8ull)
#define MMIO_MAX_MAPPINGS 64u
#define X86_RFLAGS_IF (1ull << 9)

struct mmio_mapping {
    bool in_use;
    void *returned_address;
    uint64_t virtual_base;
    uint64_t physical_base;
    size_t page_count;
    size_t first_page;
};

static const uint64_t arena_candidates[] = {
    0xffffa00000000000ull,
    0xffffa80000000000ull,
    0xffffb00000000000ull,
    0xffffb80000000000ull,
};

static uint8_t page_bitmap[MMIO_BITMAP_BYTES];
static struct mmio_mapping mappings[MMIO_MAX_MAPPINGS];
static uint64_t arena_base;
static uint64_t guard_physical;
static size_t page_hint;
static volatile uint32_t mmio_lock_word;
static bool initialized;

static void zero_bytes(void *pointer, size_t size) {
    uint8_t *bytes = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&mmio_lock_word, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&mmio_lock_word, 0u, __ATOMIC_RELEASE);
    if ((flags & X86_RFLAGS_IF) != 0) __asm__ volatile ("sti" : : : "memory");
}

static bool bit_get(size_t page) {
    return (page_bitmap[page >> 3] & (uint8_t)(1u << (page & 7u))) != 0;
}

static void bit_set(size_t page, bool value) {
    const uint8_t mask = (uint8_t)(1u << (page & 7u));
    if (value) page_bitmap[page >> 3] |= mask;
    else page_bitmap[page >> 3] &= (uint8_t)~mask;
}

static uint64_t page_virtual(size_t page) {
    return arena_base + (uint64_t)page * TWILIGHT_PAGE_SIZE;
}

static bool find_run(size_t begin, size_t end, size_t count, size_t *out) {
    size_t start = 0;
    size_t run = 0;

    for (size_t page = begin; page < end; ++page) {
        if (bit_get(page)) {
            run = 0;
            continue;
        }

        uint64_t physical = 0;
        if (vmm_translate(vmm_kernel_space(), page_virtual(page), &physical, 0)) {
            bit_set(page, true);
            run = 0;
            continue;
        }

        if (run == 0) start = page;
        ++run;
        if (run == count) {
            for (size_t i = 0; i < count; ++i) bit_set(start + i, true);
            page_hint = start + count;
            if (page_hint >= (size_t)MMIO_ARENA_PAGES) page_hint = 1;
            *out = start;
            return true;
        }
    }
    return false;
}

static bool reserve_pages(size_t count, size_t *out) {
    if (count == 0 || count >= (size_t)MMIO_ARENA_PAGES || out == 0) return false;
    if (page_hint < 1 || page_hint >= (size_t)MMIO_ARENA_PAGES) page_hint = 1;

    if (find_run(page_hint, (size_t)MMIO_ARENA_PAGES, count, out)) return true;
    if (page_hint > 1 && find_run(1, page_hint, count, out)) return true;
    return false;
}

static void release_pages(size_t first, size_t count) {
    for (size_t i = 0; i < count; ++i) bit_set(first + i, false);
    if (first < page_hint) page_hint = first;
}

static struct mmio_mapping *free_mapping_slot(void) {
    for (size_t i = 0; i < MMIO_MAX_MAPPINGS; ++i) {
        if (!mappings[i].in_use) return &mappings[i];
    }
    return 0;
}

bool mmio_init(void) {
    initialized = false;
    arena_base = 0;
    guard_physical = 0;
    page_hint = 1;
    mmio_lock_word = 0;
    zero_bytes(page_bitmap, sizeof(page_bitmap));
    zero_bytes(mappings, sizeof(mappings));

    if (!pmm_is_initialized() || !vmm_is_initialized()) return false;
    if (vmm_current_space() != vmm_kernel_space()) return false;

    const uint64_t guard = pmm_alloc_page();
    if (guard == 0) return false;

    uint64_t flags = VMM_FLAG_WRITE | VMM_FLAG_GLOBAL |
                     VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;
    if (vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;

    for (size_t i = 0; i < sizeof(arena_candidates) / sizeof(arena_candidates[0]); ++i) {
        uint64_t existing = 0;
        if (vmm_translate(vmm_kernel_space(), arena_candidates[i], &existing, 0)) continue;

        if (vmm_map_page(vmm_kernel_space(), arena_candidates[i], guard, flags)) {
            arena_base = arena_candidates[i];
            guard_physical = guard;
            bit_set(0, true);
            initialized = true;
            return true;
        }
    }

    (void)pmm_free_page(guard);
    return false;
}

bool mmio_is_initialized(void) {
    return initialized;
}

void *mmio_map(uint64_t physical_address, size_t length) {
    if (!initialized || length == 0) return 0;

    const uint64_t page_mask = TWILIGHT_PAGE_SIZE - 1ull;
    const uint64_t physical_base = physical_address & ~page_mask;
    const size_t offset = (size_t)(physical_address & page_mask);
    if (length > (size_t)-1 - offset) return 0;

    const size_t total = offset + length;
    if (total > (size_t)-1 - (TWILIGHT_PAGE_SIZE - 1ull)) return 0;
    const size_t pages = (total + TWILIGHT_PAGE_SIZE - 1ull) / TWILIGHT_PAGE_SIZE;

    const uint64_t irq_flags = lock_irqsave();
    struct mmio_mapping *mapping = free_mapping_slot();
    size_t first_page = 0;
    if (mapping == 0 || !reserve_pages(pages, &first_page)) {
        unlock_irqrestore(irq_flags);
        return 0;
    }

    const uint64_t virtual_base = page_virtual(first_page);
    uint64_t flags = VMM_FLAG_WRITE | VMM_FLAG_GLOBAL |
                     VMM_FLAG_CACHE_DISABLE | VMM_FLAG_WRITE_THROUGH;
    if (vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;

    if (!vmm_map_range(vmm_kernel_space(), virtual_base, physical_base, pages, flags)) {
        release_pages(first_page, pages);
        unlock_irqrestore(irq_flags);
        return 0;
    }

    mapping->in_use = true;
    mapping->virtual_base = virtual_base;
    mapping->physical_base = physical_base;
    mapping->page_count = pages;
    mapping->first_page = first_page;
    mapping->returned_address = (void *)(uintptr_t)(virtual_base + offset);

    void *result = mapping->returned_address;
    unlock_irqrestore(irq_flags);
    return result;
}

bool mmio_unmap(void *virtual_address) {
    if (!initialized || virtual_address == 0) return false;

    const uint64_t irq_flags = lock_irqsave();
    struct mmio_mapping *mapping = 0;
    for (size_t i = 0; i < MMIO_MAX_MAPPINGS; ++i) {
        if (mappings[i].in_use && mappings[i].returned_address == virtual_address) {
            mapping = &mappings[i];
            break;
        }
    }

    if (mapping == 0) {
        unlock_irqrestore(irq_flags);
        return false;
    }

    bool ok = true;
    for (size_t i = 0; i < mapping->page_count; ++i) {
        uint64_t old_physical = 0;
        if (!vmm_unmap_page(vmm_kernel_space(),
                            mapping->virtual_base + (uint64_t)i * TWILIGHT_PAGE_SIZE,
                            &old_physical)) {
            ok = false;
        } else if (old_physical != mapping->physical_base + (uint64_t)i * TWILIGHT_PAGE_SIZE) {
            ok = false;
        }
    }

    if (ok) {
        release_pages(mapping->first_page, mapping->page_count);
        zero_bytes(mapping, sizeof(*mapping));
    }

    unlock_irqrestore(irq_flags);
    return ok;
}
