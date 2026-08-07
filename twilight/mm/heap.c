#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/heap.h>
#include <twilight/pmm.h>
#include <twilight/vmm.h>

#define HEAP_ARENA_BYTES (1ull << 30)
#define HEAP_ARENA_PAGES (HEAP_ARENA_BYTES / TWILIGHT_PAGE_SIZE)
#define HEAP_BITMAP_BYTES ((HEAP_ARENA_PAGES + 7ull) / 8ull)
#define HEAP_ALIGNMENT 16ull
#define HEAP_SMALL_MAX 2048ull
#define HEAP_CLASS_COUNT 8u
#define SLAB_BITMAP_WORDS 4u

#define SLAB_MAGIC  0x54574c534c414231ull /* "TWLSLAB1" */
#define LARGE_MAGIC 0x54574c4c41524731ull /* "TWLLARG1" */

#define X86_RFLAGS_IF (1ull << 9)

enum allocation_kind {
    ALLOCATION_INVALID = 0,
    ALLOCATION_SLAB,
    ALLOCATION_LARGE,
};

struct slab_page {
    uint64_t magic;
    uint64_t physical_address;
    struct slab_page *next;
    struct slab_page *previous;
    void *free_list;
    uint16_t class_index;
    uint16_t capacity;
    uint16_t free_count;
    uint16_t data_offset;
    uint64_t allocated[SLAB_BITMAP_WORDS];
};

struct large_header {
    uint64_t magic;
    uint64_t physical_address;
    uint64_t virtual_address;
    uint64_t requested_size;
    uint64_t page_count;
};

struct allocation_info {
    enum allocation_kind kind;
    size_t usable_size;
    size_t copy_size;
    size_t page_index;
    uint16_t class_index;
    uint16_t slot_index;
    struct slab_page *slab;
    struct large_header *large;
};

static const size_t class_sizes[HEAP_CLASS_COUNT] = {
    16u, 32u, 64u, 128u, 256u, 512u, 1024u, 2048u,
};

/*
 * Prefer PML4 slots well away from the usual Limine HHDM and high kernel
 * image. Every allocation still checks vmm_translate(), so an unexpected
 * pre-existing mapping is skipped rather than overwritten.
 */
static const uint64_t heap_base_candidates[] = {
    0xffffc00000000000ull,
    0xffffc80000000000ull,
    0xffffd00000000000ull,
    0xffffd80000000000ull,
    0xffffe00000000000ull,
    0xffffe80000000000ull,
    0xfffff00000000000ull,
    0xfffff80000000000ull,
};

static struct slab_page *class_slabs[HEAP_CLASS_COUNT];
static uint8_t page_bitmap[HEAP_BITMAP_BYTES];
static struct heap_stats stats;
static uint64_t heap_base;
static uint64_t guard_physical;
static size_t virtual_hint;
static volatile uint32_t heap_lock_word;
static bool initialized;

static void bytes_zero(void *pointer, size_t size) {
    uint8_t *out = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) out[i] = 0;
}

static void bytes_fill(void *pointer, uint8_t value, size_t size) {
    uint8_t *out = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) out[i] = value;
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

static bool align_up_size(size_t value, size_t alignment, size_t *out) {
    if (alignment == 0 || out == 0) return false;
    const size_t mask = alignment - 1u;
    if ((alignment & mask) != 0) return false;
    if (value > (size_t)-1 - mask) return false;
    *out = (value + mask) & ~mask;
    return true;
}

static uint64_t heap_page_flags(void) {
    uint64_t flags = VMM_FLAG_WRITE | VMM_FLAG_GLOBAL;
    if (vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;
    return flags;
}

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");

    while (__atomic_exchange_n(&heap_lock_word, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&heap_lock_word, 0u, __ATOMIC_RELEASE);
    if ((flags & X86_RFLAGS_IF) != 0) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static bool page_bit_get(size_t page) {
    return (page_bitmap[page >> 3] & (uint8_t)(1u << (page & 7u))) != 0;
}

static void page_bit_set(size_t page, bool value) {
    const uint8_t mask = (uint8_t)(1u << (page & 7u));
    if (value) page_bitmap[page >> 3] |= mask;
    else page_bitmap[page >> 3] &= (uint8_t)~mask;
}

static uint64_t page_virtual_address(size_t page) {
    return heap_base + (uint64_t)page * TWILIGHT_PAGE_SIZE;
}

static bool virtual_page_is_mapped(size_t page) {
    uint64_t physical = 0;
    return vmm_translate(vmm_kernel_space(), page_virtual_address(page), &physical, 0);
}

static bool find_run_in_range(size_t begin, size_t end, size_t count, size_t *out) {
    size_t run_start = 0;
    size_t run_length = 0;

    for (size_t page = begin; page < end; ++page) {
        if (page_bit_get(page)) {
            run_length = 0;
            continue;
        }

        /* Never overwrite a mapping that exists outside the heap allocator. */
        if (virtual_page_is_mapped(page)) {
            page_bit_set(page, true);
            run_length = 0;
            continue;
        }

        if (run_length == 0) run_start = page;
        ++run_length;

        if (run_length == count) {
            for (size_t i = 0; i < count; ++i) page_bit_set(run_start + i, true);
            virtual_hint = run_start + count;
            if (virtual_hint >= (size_t)HEAP_ARENA_PAGES) virtual_hint = 1;
            *out = run_start;
            return true;
        }
    }

    return false;
}

static bool reserve_virtual_pages(size_t count, size_t *out) {
    if (count == 0 || out == 0 || count >= (size_t)HEAP_ARENA_PAGES) return false;

    if (virtual_hint < 1 || virtual_hint >= (size_t)HEAP_ARENA_PAGES) virtual_hint = 1;

    if (find_run_in_range(virtual_hint, (size_t)HEAP_ARENA_PAGES, count, out)) return true;
    if (virtual_hint > 1 && find_run_in_range(1, virtual_hint, count, out)) return true;
    return false;
}

static void release_virtual_pages(size_t first, size_t count) {
    if (first == 0 || count == 0 || first >= (size_t)HEAP_ARENA_PAGES) return;
    if (count > (size_t)HEAP_ARENA_PAGES - first) return;

    for (size_t i = 0; i < count; ++i) page_bit_set(first + i, false);
    if (first < virtual_hint) virtual_hint = first;
}

static int class_for_size(size_t size) {
    for (unsigned i = 0; i < HEAP_CLASS_COUNT; ++i) {
        if (size <= class_sizes[i]) return (int)i;
    }
    return -1;
}

static bool slab_slot_bit_get(const struct slab_page *slab, size_t slot) {
    if (slot >= slab->capacity) return false;
    return (slab->allocated[slot >> 6] & (1ull << (slot & 63u))) != 0;
}

static void slab_slot_bit_set(struct slab_page *slab, size_t slot, bool value) {
    const uint64_t mask = 1ull << (slot & 63u);
    if (value) slab->allocated[slot >> 6] |= mask;
    else slab->allocated[slot >> 6] &= ~mask;
}

static struct slab_page *create_slab(unsigned class_index) {
    size_t virtual_page = 0;
    if (!reserve_virtual_pages(1, &virtual_page)) return 0;

    const uint64_t physical = pmm_alloc_page();
    if (physical == 0) {
        release_virtual_pages(virtual_page, 1);
        return 0;
    }

    const uint64_t virtual_address = page_virtual_address(virtual_page);
    if (!vmm_map_page(vmm_kernel_space(), virtual_address, physical, heap_page_flags())) {
        (void)pmm_free_page(physical);
        release_virtual_pages(virtual_page, 1);
        return 0;
    }

    struct slab_page *slab = (struct slab_page *)(uintptr_t)virtual_address;
    bytes_zero(slab, sizeof(*slab));

    size_t data_offset = 0;
    if (!align_up_size(sizeof(*slab), (size_t)HEAP_ALIGNMENT, &data_offset)) {
        (void)vmm_unmap_page(vmm_kernel_space(), virtual_address, 0);
        (void)pmm_free_page(physical);
        release_virtual_pages(virtual_page, 1);
        return 0;
    }

    const size_t slot_size = class_sizes[class_index];
    const size_t capacity = (TWILIGHT_PAGE_SIZE - data_offset) / slot_size;
    if (capacity == 0 || capacity > SLAB_BITMAP_WORDS * 64u) {
        (void)vmm_unmap_page(vmm_kernel_space(), virtual_address, 0);
        (void)pmm_free_page(physical);
        release_virtual_pages(virtual_page, 1);
        return 0;
    }

    slab->magic = SLAB_MAGIC;
    slab->physical_address = physical;
    slab->class_index = (uint16_t)class_index;
    slab->capacity = (uint16_t)capacity;
    slab->free_count = (uint16_t)capacity;
    slab->data_offset = (uint16_t)data_offset;

    uint8_t *page = (uint8_t *)slab;
    void *free_list = 0;
    for (size_t i = capacity; i > 0; --i) {
        void *slot = page + data_offset + (i - 1u) * slot_size;
        *(void **)slot = free_list;
        free_list = slot;
    }
    slab->free_list = free_list;

    slab->next = class_slabs[class_index];
    if (slab->next != 0) slab->next->previous = slab;
    class_slabs[class_index] = slab;

    ++stats.slab_pages;
    ++stats.mapped_pages;
    return slab;
}

static void *allocate_small_locked(size_t size) {
    const int class_index = class_for_size(size);
    if (class_index < 0) return 0;

    struct slab_page *slab = class_slabs[class_index];
    while (slab != 0 && slab->free_count == 0) slab = slab->next;
    if (slab == 0) slab = create_slab((unsigned)class_index);
    if (slab == 0 || slab->free_list == 0 || slab->free_count == 0) return 0;

    void *slot = slab->free_list;
    slab->free_list = *(void **)slot;

    const size_t slot_size = class_sizes[class_index];
    const uintptr_t data_start = (uintptr_t)slab + slab->data_offset;
    const size_t slot_index = ((uintptr_t)slot - data_start) / slot_size;
    if (slot_index >= slab->capacity || slab_slot_bit_get(slab, slot_index)) return 0;

    slab_slot_bit_set(slab, slot_index, true);
    --slab->free_count;

    ++stats.active_allocations;
    stats.allocated_bytes += slot_size;
    return slot;
}

static size_t large_user_offset(void) {
    size_t offset = 0;
    if (!align_up_size(sizeof(struct large_header), (size_t)HEAP_ALIGNMENT, &offset)) return 0;
    return offset;
}

static bool large_pages_for_size(size_t size, size_t *page_count, size_t *user_offset) {
    const size_t offset = large_user_offset();
    if (offset == 0 || size > (size_t)-1 - offset) return false;

    const size_t total = offset + size;
    if (total > (size_t)-1 - (TWILIGHT_PAGE_SIZE - 1ull)) return false;

    const size_t pages = (total + TWILIGHT_PAGE_SIZE - 1ull) / TWILIGHT_PAGE_SIZE;
    if (pages == 0 || pages >= (size_t)HEAP_ARENA_PAGES) return false;

    *page_count = pages;
    *user_offset = offset;
    return true;
}

static void *allocate_large_locked(size_t size) {
    size_t pages = 0;
    size_t user_offset = 0;
    if (!large_pages_for_size(size, &pages, &user_offset)) return 0;

    size_t virtual_page = 0;
    if (!reserve_virtual_pages(pages, &virtual_page)) return 0;

    const uint64_t physical = pmm_alloc_pages(pages);
    if (physical == 0) {
        release_virtual_pages(virtual_page, pages);
        return 0;
    }

    const uint64_t virtual_address = page_virtual_address(virtual_page);
    if (!vmm_map_range(vmm_kernel_space(), virtual_address, physical, pages, heap_page_flags())) {
        (void)pmm_free_pages(physical, pages);
        release_virtual_pages(virtual_page, pages);
        return 0;
    }

    struct large_header *header = (struct large_header *)(uintptr_t)virtual_address;
    header->magic = LARGE_MAGIC;
    header->physical_address = physical;
    header->virtual_address = virtual_address;
    header->requested_size = size;
    header->page_count = pages;

    const size_t usable = pages * TWILIGHT_PAGE_SIZE - user_offset;
    ++stats.active_allocations;
    ++stats.large_allocations;
    stats.large_pages += pages;
    stats.mapped_pages += pages;
    stats.allocated_bytes += usable;

    return (void *)(uintptr_t)(virtual_address + user_offset);
}

static bool pointer_page_index(const void *pointer, size_t *page_index) {
    if (pointer == 0 || heap_base == 0 || page_index == 0) return false;

    const uint64_t address = (uint64_t)(uintptr_t)pointer;
    if (address < heap_base || address >= heap_base + HEAP_ARENA_BYTES) return false;

    const size_t page = (size_t)((address - heap_base) / TWILIGHT_PAGE_SIZE);
    if (page == 0 || page >= (size_t)HEAP_ARENA_PAGES) return false;
    if (!page_bit_get(page)) return false;

    *page_index = page;
    return true;
}

static bool query_allocation_locked(const void *pointer, struct allocation_info *out) {
    if (out == 0) return false;
    *out = (struct allocation_info){0};

    size_t page_index = 0;
    if (!pointer_page_index(pointer, &page_index)) return false;

    const uint64_t page_address = page_virtual_address(page_index);
    const uint64_t magic = *(const uint64_t *)(uintptr_t)page_address;

    if (magic == SLAB_MAGIC) {
        struct slab_page *slab = (struct slab_page *)(uintptr_t)page_address;
        if (slab->class_index >= HEAP_CLASS_COUNT || slab->capacity == 0) return false;

        const size_t slot_size = class_sizes[slab->class_index];
        const uintptr_t data_start = (uintptr_t)slab + slab->data_offset;
        const uintptr_t address = (uintptr_t)pointer;
        if (address < data_start) return false;

        const size_t delta = address - data_start;
        if ((delta % slot_size) != 0) return false;

        const size_t slot_index = delta / slot_size;
        if (slot_index >= slab->capacity || !slab_slot_bit_get(slab, slot_index)) return false;

        out->kind = ALLOCATION_SLAB;
        out->usable_size = slot_size;
        out->copy_size = slot_size;
        out->page_index = page_index;
        out->class_index = slab->class_index;
        out->slot_index = (uint16_t)slot_index;
        out->slab = slab;
        return true;
    }

    if (magic == LARGE_MAGIC) {
        struct large_header *large = (struct large_header *)(uintptr_t)page_address;
        const size_t user_offset = large_user_offset();
        if (user_offset == 0 || large->virtual_address != page_address) return false;
        if (large->page_count == 0 || large->page_count > HEAP_ARENA_PAGES - page_index) return false;
        if ((uintptr_t)pointer != (uintptr_t)large + user_offset) return false;

        const size_t usable = (size_t)large->page_count * TWILIGHT_PAGE_SIZE - user_offset;
        if (large->requested_size > usable) return false;

        out->kind = ALLOCATION_LARGE;
        out->usable_size = usable;
        out->copy_size = (size_t)large->requested_size;
        out->page_index = page_index;
        out->large = large;
        return true;
    }

    return false;
}

static bool destroy_empty_slab_locked(struct slab_page *slab, size_t page_index) {
    const unsigned class_index = slab->class_index;
    const uint64_t physical = slab->physical_address;
    const uint64_t virtual_address = page_virtual_address(page_index);
    struct slab_page *previous = slab->previous;
    struct slab_page *next = slab->next;

    if (previous != 0) previous->next = next;
    else class_slabs[class_index] = next;
    if (next != 0) next->previous = previous;

    uint64_t unmapped_physical = 0;
    if (!vmm_unmap_page(vmm_kernel_space(), virtual_address, &unmapped_physical)) return false;
    if (unmapped_physical != physical) return false;
    if (!pmm_free_page(physical)) return false;

    release_virtual_pages(page_index, 1);
    --stats.slab_pages;
    --stats.mapped_pages;
    return true;
}

static bool free_small_locked(struct allocation_info *info, void *pointer) {
    struct slab_page *slab = info->slab;
    const size_t slot_size = class_sizes[info->class_index];

    if (!slab_slot_bit_get(slab, info->slot_index)) return false;
    slab_slot_bit_set(slab, info->slot_index, false);

    bytes_fill(pointer, 0xa5u, slot_size);
    *(void **)pointer = slab->free_list;
    slab->free_list = pointer;
    ++slab->free_count;

    --stats.active_allocations;
    stats.allocated_bytes -= slot_size;

    if (slab->free_count == slab->capacity) {
        return destroy_empty_slab_locked(slab, info->page_index);
    }
    return true;
}

static bool free_large_locked(struct allocation_info *info) {
    const uint64_t physical = info->large->physical_address;
    const size_t pages = (size_t)info->large->page_count;
    const size_t usable = info->usable_size;
    const size_t first_page = info->page_index;
    const uint64_t virtual_address = page_virtual_address(first_page);

    bool unmapped_all = true;
    for (size_t i = 0; i < pages; ++i) {
        uint64_t unmapped_physical = 0;
        if (!vmm_unmap_page(vmm_kernel_space(),
                            virtual_address + (uint64_t)i * TWILIGHT_PAGE_SIZE,
                            &unmapped_physical)) {
            unmapped_all = false;
        }
    }
    if (!unmapped_all) return false;
    if (!pmm_free_pages(physical, pages)) return false;

    release_virtual_pages(first_page, pages);
    --stats.active_allocations;
    --stats.large_allocations;
    stats.large_pages -= pages;
    stats.mapped_pages -= pages;
    stats.allocated_bytes -= usable;
    return true;
}

bool heap_init(void) {
    initialized = false;
    heap_base = 0;
    guard_physical = 0;
    virtual_hint = 1;
    heap_lock_word = 0;

    if (!pmm_is_initialized() || !vmm_is_initialized()) return false;
    if (vmm_current_space() != vmm_kernel_space()) return false;

    bytes_zero(class_slabs, sizeof(class_slabs));
    bytes_zero(page_bitmap, sizeof(page_bitmap));
    bytes_zero(&stats, sizeof(stats));
    stats.arena_bytes = HEAP_ARENA_BYTES;

    const uint64_t guard = pmm_alloc_page();
    if (guard == 0) return false;

    for (size_t i = 0; i < sizeof(heap_base_candidates) / sizeof(heap_base_candidates[0]); ++i) {
        const uint64_t candidate = heap_base_candidates[i];
        uint64_t existing = 0;
        if (vmm_translate(vmm_kernel_space(), candidate, &existing, 0)) continue;

        if (vmm_map_page(vmm_kernel_space(), candidate, guard, heap_page_flags())) {
            heap_base = candidate;
            guard_physical = guard;
            page_bit_set(0, true);
            stats.mapped_pages = 1;
            initialized = true;
            return true;
        }
    }

    (void)pmm_free_page(guard);
    return false;
}

bool heap_is_initialized(void) {
    return initialized;
}

void *kmalloc(size_t size) {
    if (!initialized || size == 0) return 0;

    const uint64_t irq_flags = lock_irqsave();
    void *result = size <= HEAP_SMALL_MAX
        ? allocate_small_locked(size)
        : allocate_large_locked(size);
    unlock_irqrestore(irq_flags);
    return result;
}

void kfree(void *pointer) {
    if (pointer == 0 || !initialized) return;

    const uint64_t irq_flags = lock_irqsave();
    struct allocation_info info;
    if (!query_allocation_locked(pointer, &info)) {
        ++stats.invalid_frees;
        unlock_irqrestore(irq_flags);
        return;
    }

    bool ok = false;
    if (info.kind == ALLOCATION_SLAB) ok = free_small_locked(&info, pointer);
    else if (info.kind == ALLOCATION_LARGE) ok = free_large_locked(&info);

    if (!ok) ++stats.invalid_frees;
    unlock_irqrestore(irq_flags);
}

void *kcalloc(size_t count, size_t size) {
    if (count == 0 || size == 0) return 0;
    if (count > (size_t)-1 / size) return 0;

    const size_t total = count * size;
    void *pointer = kmalloc(total);
    if (pointer != 0) bytes_zero(pointer, total);
    return pointer;
}

size_t ksize(const void *pointer) {
    if (pointer == 0 || !initialized) return 0;

    const uint64_t irq_flags = lock_irqsave();
    struct allocation_info info;
    const size_t result = query_allocation_locked(pointer, &info) ? info.usable_size : 0;
    unlock_irqrestore(irq_flags);
    return result;
}

void *krealloc(void *pointer, size_t new_size) {
    if (pointer == 0) return kmalloc(new_size);
    if (new_size == 0) {
        kfree(pointer);
        return 0;
    }
    if (!initialized) return 0;

    struct allocation_info info;
    bool same_bucket = false;

    const uint64_t irq_flags = lock_irqsave();
    if (!query_allocation_locked(pointer, &info)) {
        unlock_irqrestore(irq_flags);
        return 0;
    }

    if (info.kind == ALLOCATION_SLAB) {
        const int new_class = class_for_size(new_size);
        same_bucket = new_class >= 0 && (uint16_t)new_class == info.class_index;
    } else if (info.kind == ALLOCATION_LARGE && new_size > HEAP_SMALL_MAX) {
        size_t new_pages = 0;
        size_t ignored_offset = 0;
        if (large_pages_for_size(new_size, &new_pages, &ignored_offset) &&
            new_pages == info.large->page_count) {
            info.large->requested_size = new_size;
            same_bucket = true;
        }
    }

    const size_t copy_size = info.copy_size < new_size ? info.copy_size : new_size;
    unlock_irqrestore(irq_flags);

    if (same_bucket) return pointer;

    void *replacement = kmalloc(new_size);
    if (replacement == 0) return 0;

    bytes_copy(replacement, pointer, copy_size);
    kfree(pointer);
    return replacement;
}

void heap_get_stats(struct heap_stats *out) {
    if (out == 0) return;

    const uint64_t irq_flags = lock_irqsave();
    *out = stats;
    unlock_irqrestore(irq_flags);
}

bool heap_self_test(void) {
    if (!initialized) return false;

    struct pmm_stats pmm_before;
    struct pmm_stats pmm_after;
    struct heap_stats heap_before;
    struct heap_stats heap_after;
    pmm_get_stats(&pmm_before);
    heap_get_stats(&heap_before);

    bool success = false;
    uint8_t *tiny = 0;
    uint8_t *medium = 0;
    uint8_t *zeroed = 0;
    uint8_t *large = 0;

    tiny = (uint8_t *)kmalloc(1);
    medium = (uint8_t *)kmalloc(128);
    zeroed = (uint8_t *)kcalloc(97, 3);
    large = (uint8_t *)kmalloc(8193);
    if (tiny == 0 || medium == 0 || zeroed == 0 || large == 0) goto cleanup;

    if (((uintptr_t)tiny & (HEAP_ALIGNMENT - 1ull)) != 0 ||
        ((uintptr_t)medium & (HEAP_ALIGNMENT - 1ull)) != 0 ||
        ((uintptr_t)zeroed & (HEAP_ALIGNMENT - 1ull)) != 0 ||
        ((uintptr_t)large & (HEAP_ALIGNMENT - 1ull)) != 0) goto cleanup;

    if (ksize(tiny) < 1 || ksize(medium) < 128 || ksize(zeroed) < 291 || ksize(large) < 8193) {
        goto cleanup;
    }

    tiny[0] = 0x5au;
    for (size_t i = 0; i < 128; ++i) medium[i] = (uint8_t)(i ^ 0xa5u);
    for (size_t i = 0; i < 291; ++i) {
        if (zeroed[i] != 0) goto cleanup;
    }
    for (size_t i = 0; i < 8193; ++i) large[i] = (uint8_t)(i * 17u + 3u);

    uint8_t *grown_medium = (uint8_t *)krealloc(medium, 1000);
    if (grown_medium == 0) goto cleanup;
    medium = grown_medium;
    for (size_t i = 0; i < 128; ++i) {
        if (medium[i] != (uint8_t)(i ^ 0xa5u)) goto cleanup;
    }

    uint8_t *shrunk_medium = (uint8_t *)krealloc(medium, 32);
    if (shrunk_medium == 0) goto cleanup;
    medium = shrunk_medium;
    for (size_t i = 0; i < 32; ++i) {
        if (medium[i] != (uint8_t)(i ^ 0xa5u)) goto cleanup;
    }

    uint8_t *grown_large = (uint8_t *)krealloc(large, 20000);
    if (grown_large == 0) goto cleanup;
    large = grown_large;
    for (size_t i = 0; i < 8193; ++i) {
        if (large[i] != (uint8_t)(i * 17u + 3u)) goto cleanup;
    }

    uint8_t *shrunk_large = (uint8_t *)krealloc(large, 64);
    if (shrunk_large == 0) goto cleanup;
    large = shrunk_large;
    for (size_t i = 0; i < 64; ++i) {
        if (large[i] != (uint8_t)(i * 17u + 3u)) goto cleanup;
    }

    if (tiny[0] != 0x5au) goto cleanup;
    success = true;

cleanup:
    kfree(tiny);
    kfree(medium);
    kfree(zeroed);
    kfree(large);

    pmm_get_stats(&pmm_after);
    heap_get_stats(&heap_after);

    if (!success) return false;
    if (pmm_after.free_pages != pmm_before.free_pages) return false;
    if (heap_after.active_allocations != heap_before.active_allocations) return false;
    if (heap_after.allocated_bytes != heap_before.allocated_bytes) return false;
    if (heap_after.slab_pages != heap_before.slab_pages) return false;
    if (heap_after.large_allocations != heap_before.large_allocations) return false;
    if (heap_after.large_pages != heap_before.large_pages) return false;
    if (heap_after.mapped_pages != heap_before.mapped_pages) return false;
    if (heap_after.invalid_frees != heap_before.invalid_frees) return false;

    return true;
}
