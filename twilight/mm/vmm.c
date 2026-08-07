#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/pmm.h>
#include <twilight/vmm.h>

#define PAGE_ENTRIES 512u

#define PTE_PRESENT       (1ull << 0)
#define PTE_WRITE         (1ull << 1)
#define PTE_USER          (1ull << 2)
#define PTE_WRITE_THROUGH (1ull << 3)
#define PTE_CACHE_DISABLE (1ull << 4)
#define PTE_ACCESSED      (1ull << 5)
#define PTE_DIRTY         (1ull << 6)
#define PTE_HUGE          (1ull << 7)
#define PTE_GLOBAL        (1ull << 8)
#define PTE_NO_EXECUTE    (1ull << 63)

#define PTE_ADDRESS_MASK 0x000ffffffffff000ull
#define PAGE_2M_MASK     0x000fffffffe00000ull
#define PAGE_1G_MASK     0x000fffffc0000000ull

#define CR4_LA57 (1ull << 12)

#define IA32_EFER_MSR 0xc0000080u
#define IA32_EFER_NXE (1ull << 11)

struct walk_path {
    uint64_t *pml4e;
    uint64_t *pdpte;
    uint64_t *pde;
    uint64_t *pte;

    uint64_t pdpt_phys;
    uint64_t pd_phys;
    uint64_t pt_phys;

    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    bool new_pdpt;
    bool new_pd;
    bool new_pt;
};

static bool initialized;
static bool nx_available;
static vmm_space_t kernel_root_phys;

static void cpuid(uint32_t leaf,
                  uint32_t subleaf,
                  uint32_t *eax,
                  uint32_t *ebx,
                  uint32_t *ecx,
                  uint32_t *edx) {
    __asm__ volatile (
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void wrmsr(uint32_t msr, uint64_t value) {
    const uint32_t low = (uint32_t)value;
    const uint32_t high = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high) : "memory");
}

static uint64_t read_cr3(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(value));
    return value;
}

static uint64_t read_cr4(void) {
    uint64_t value;
    __asm__ volatile ("mov %%cr4, %0" : "=r"(value));
    return value;
}

static void write_cr3(uint64_t value) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(value) : "memory");
}

static void invalidate_page(uint64_t virtual_address) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

static bool is_canonical_48(uint64_t virtual_address) {
    const uint64_t upper = virtual_address >> 48;
    const bool sign = ((virtual_address >> 47) & 1ull) != 0;
    return sign ? upper == 0xffffull : upper == 0;
}

static bool is_upper_half(uint64_t virtual_address) {
    return ((virtual_address >> 47) & 1ull) != 0;
}

static uint64_t table_index(uint64_t virtual_address, unsigned shift) {
    return (virtual_address >> shift) & 0x1ffull;
}

static uint64_t *table_from_phys(uint64_t physical_address) {
    if ((physical_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return 0;
    return (uint64_t *)pmm_phys_to_virt(physical_address);
}

static void zero_table(uint64_t *table) {
    for (size_t i = 0; i < PAGE_ENTRIES; ++i) table[i] = 0;
}

static bool table_is_empty(const uint64_t *table) {
    for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
        if (table[i] != 0) return false;
    }
    return true;
}

static uint64_t mapping_bits(uint64_t flags) {
    uint64_t bits = PTE_PRESENT;
    if ((flags & VMM_FLAG_WRITE) != 0) bits |= PTE_WRITE;
    if ((flags & VMM_FLAG_USER) != 0) bits |= PTE_USER;
    if ((flags & VMM_FLAG_WRITE_THROUGH) != 0) bits |= PTE_WRITE_THROUGH;
    if ((flags & VMM_FLAG_CACHE_DISABLE) != 0) bits |= PTE_CACHE_DISABLE;
    if ((flags & VMM_FLAG_GLOBAL) != 0) bits |= PTE_GLOBAL;
    if ((flags & VMM_FLAG_NO_EXECUTE) != 0) bits |= PTE_NO_EXECUTE;
    return bits;
}

static bool flags_supported(uint64_t flags) {
    const uint64_t known = VMM_FLAG_WRITE |
                           VMM_FLAG_USER |
                           VMM_FLAG_WRITE_THROUGH |
                           VMM_FLAG_CACHE_DISABLE |
                           VMM_FLAG_GLOBAL |
                           VMM_FLAG_NO_EXECUTE;
    if ((flags & ~known) != 0) return false;
    if ((flags & VMM_FLAG_NO_EXECUTE) != 0 && !nx_available) return false;
    return true;
}

static bool valid_space(vmm_space_t space) {
    if (space == VMM_INVALID_SPACE) return false;
    if ((space & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
    if ((space & ~PTE_ADDRESS_MASK) != 0) return false;
    return table_from_phys(space) != 0;
}

static bool allocate_child_table(uint64_t *parent_entry,
                                 bool user,
                                 uint64_t *physical_out,
                                 uint64_t **virtual_out) {
    const uint64_t physical = pmm_alloc_page();
    if (physical == 0) return false;

    uint64_t *table = table_from_phys(physical);
    if (table == 0) {
        (void)pmm_free_page(physical);
        return false;
    }

    zero_table(table);

    uint64_t bits = PTE_PRESENT | PTE_WRITE;
    if (user) bits |= PTE_USER;
    *parent_entry = physical | bits;

    *physical_out = physical;
    *virtual_out = table;
    return true;
}

static void rollback_created_tables(struct walk_path *path) {
    if (path->new_pt) {
        *path->pde = 0;
        (void)pmm_free_page(path->pt_phys);
        path->new_pt = false;
    }
    if (path->new_pd) {
        *path->pdpte = 0;
        (void)pmm_free_page(path->pd_phys);
        path->new_pd = false;
    }
    if (path->new_pdpt) {
        *path->pml4e = 0;
        (void)pmm_free_page(path->pdpt_phys);
        path->new_pdpt = false;
    }
}

static bool walk_to_pte(vmm_space_t space,
                        uint64_t virtual_address,
                        bool create,
                        bool user,
                        struct walk_path *path) {
    uint64_t *pml4 = table_from_phys(space);
    if (pml4 == 0 || path == 0) return false;

    *path = (struct walk_path){0};

    path->pml4e = &pml4[table_index(virtual_address, 39)];
    if ((*path->pml4e & PTE_PRESENT) == 0) {
        if (!create) return false;
        if (!allocate_child_table(path->pml4e, user, &path->pdpt_phys, &path->pdpt)) return false;
        path->new_pdpt = true;
    } else {
        path->pdpt_phys = *path->pml4e & PTE_ADDRESS_MASK;
        path->pdpt = table_from_phys(path->pdpt_phys);
        if (path->pdpt == 0) return false;
    }

    path->pdpte = &path->pdpt[table_index(virtual_address, 30)];
    if ((*path->pdpte & PTE_PRESENT) != 0 && (*path->pdpte & PTE_HUGE) != 0) {
        rollback_created_tables(path);
        return false;
    }

    if ((*path->pdpte & PTE_PRESENT) == 0) {
        if (!create) {
            rollback_created_tables(path);
            return false;
        }
        if (!allocate_child_table(path->pdpte, user, &path->pd_phys, &path->pd)) {
            rollback_created_tables(path);
            return false;
        }
        path->new_pd = true;
    } else {
        path->pd_phys = *path->pdpte & PTE_ADDRESS_MASK;
        path->pd = table_from_phys(path->pd_phys);
        if (path->pd == 0) {
            rollback_created_tables(path);
            return false;
        }
    }

    path->pde = &path->pd[table_index(virtual_address, 21)];
    if ((*path->pde & PTE_PRESENT) != 0 && (*path->pde & PTE_HUGE) != 0) {
        rollback_created_tables(path);
        return false;
    }

    if ((*path->pde & PTE_PRESENT) == 0) {
        if (!create) {
            rollback_created_tables(path);
            return false;
        }
        if (!allocate_child_table(path->pde, user, &path->pt_phys, &path->pt)) {
            rollback_created_tables(path);
            return false;
        }
        path->new_pt = true;
    } else {
        path->pt_phys = *path->pde & PTE_ADDRESS_MASK;
        path->pt = table_from_phys(path->pt_phys);
        if (path->pt == 0) {
            rollback_created_tables(path);
            return false;
        }
    }

    path->pte = &path->pt[table_index(virtual_address, 12)];
    return true;
}

static bool current_space_is(vmm_space_t space) {
    return (read_cr3() & PTE_ADDRESS_MASK) == space;
}

static bool try_release_empty_table(uint64_t *parent_entry,
                                    uint64_t *table,
                                    uint64_t physical_address) {
    if (!table_is_empty(table)) return false;

    const uint64_t saved = *parent_entry;
    *parent_entry = 0;

    if (!pmm_free_page(physical_address)) {
        *parent_entry = saved;
        return false;
    }
    return true;
}

static uint64_t decoded_leaf_flags(uint64_t leaf,
                                   bool effective_write,
                                   bool effective_user,
                                   bool effective_nx) {
    uint64_t flags = 0;
    if (effective_write) flags |= VMM_FLAG_WRITE;
    if (effective_user) flags |= VMM_FLAG_USER;
    if ((leaf & PTE_WRITE_THROUGH) != 0) flags |= VMM_FLAG_WRITE_THROUGH;
    if ((leaf & PTE_CACHE_DISABLE) != 0) flags |= VMM_FLAG_CACHE_DISABLE;
    if ((leaf & PTE_GLOBAL) != 0) flags |= VMM_FLAG_GLOBAL;
    if (effective_nx) flags |= VMM_FLAG_NO_EXECUTE;
    return flags;
}

static void restrict_effective_permissions(uint64_t entry,
                                           bool *write,
                                           bool *user,
                                           bool *no_execute) {
    if ((entry & PTE_WRITE) == 0) *write = false;
    if ((entry & PTE_USER) == 0) *user = false;
    if (nx_available && (entry & PTE_NO_EXECUTE) != 0) *no_execute = true;
}

static bool free_table_tree(uint64_t table_phys, unsigned level) {
    uint64_t *table = table_from_phys(table_phys);
    if (table == 0 || level == 0 || level > 3) return false;

    bool ok = true;

    if (level > 1) {
        for (size_t i = 0; i < PAGE_ENTRIES; ++i) {
            const uint64_t entry = table[i];
            if ((entry & PTE_PRESENT) == 0) continue;

            /* At PDPT/PD levels a huge entry maps data directly, not a table. */
            if ((entry & PTE_HUGE) != 0) continue;

            const uint64_t child = entry & PTE_ADDRESS_MASK;
            if (!free_table_tree(child, level - 1)) ok = false;
        }
    }

    if (!pmm_free_page(table_phys)) ok = false;
    return ok;
}

static bool find_self_test_virtual_address(uint64_t *out) {
    if (out == 0) return false;

    uint64_t *root = table_from_phys(kernel_root_phys);
    if (root == 0) return false;

    /*
     * Pick an entirely unused upper-half PML4 slot. That guarantees the test
     * cannot collide with Limine's kernel/HHDM mappings and makes cleanup able
     * to reclaim every page-table page it creates.
     */
    for (uint64_t index = 256; index < 511; ++index) {
        if ((root[index] & PTE_PRESENT) != 0) continue;

        uint64_t virtual_address = (index << 39) | 0xffff000000000000ull;
        virtual_address += 0x200000ull;
        if (!is_canonical_48(virtual_address)) continue;

        *out = virtual_address;
        return true;
    }

    return false;
}

bool vmm_init(void) {
    initialized = false;
    nx_available = false;
    kernel_root_phys = VMM_INVALID_SPACE;

    if (!pmm_is_initialized()) return false;

    /* Twilight currently implements the normal four-level x86_64 page walk. */
    if ((read_cr4() & CR4_LA57) != 0) return false;

    kernel_root_phys = read_cr3() & PTE_ADDRESS_MASK;
    if (!valid_space(kernel_root_phys)) {
        kernel_root_phys = VMM_INVALID_SPACE;
        return false;
    }

    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    cpuid(0x80000000u, 0, &eax, &ebx, &ecx, &edx);
    if (eax >= 0x80000001u) {
        cpuid(0x80000001u, 0, &eax, &ebx, &ecx, &edx);
        if ((edx & (1u << 20)) != 0) {
            uint64_t efer = rdmsr(IA32_EFER_MSR);
            efer |= IA32_EFER_NXE;
            wrmsr(IA32_EFER_MSR, efer);
            nx_available = true;
        }
    }

    initialized = true;
    return true;
}

bool vmm_is_initialized(void) {
    return initialized;
}

bool vmm_nx_supported(void) {
    return initialized && nx_available;
}

vmm_space_t vmm_kernel_space(void) {
    return initialized ? kernel_root_phys : VMM_INVALID_SPACE;
}

vmm_space_t vmm_current_space(void) {
    if (!initialized) return VMM_INVALID_SPACE;
    return read_cr3() & PTE_ADDRESS_MASK;
}

vmm_space_t vmm_create_address_space(void) {
    if (!initialized) return VMM_INVALID_SPACE;

    const uint64_t root_phys = pmm_alloc_page();
    if (root_phys == 0) return VMM_INVALID_SPACE;

    uint64_t *root = table_from_phys(root_phys);
    uint64_t *kernel_root = table_from_phys(kernel_root_phys);
    if (root == 0 || kernel_root == 0) {
        (void)pmm_free_page(root_phys);
        return VMM_INVALID_SPACE;
    }

    zero_table(root);

    /*
     * User address spaces start empty below bit 47 and share Twilight's upper
     * half mappings. The referenced kernel page-table trees remain shared;
     * only this new PML4 page is private.
     */
    for (size_t i = 256; i < PAGE_ENTRIES; ++i) root[i] = kernel_root[i];

    return root_phys;
}

bool vmm_destroy_address_space(vmm_space_t space) {
    if (!initialized || !valid_space(space)) return false;
    if (space == kernel_root_phys || space == vmm_current_space()) return false;

    uint64_t *root = table_from_phys(space);
    if (root == 0) return false;

    bool ok = true;

    /* Only lower-half trees are private. Upper-half kernel mappings are shared. */
    for (size_t i = 0; i < 256; ++i) {
        const uint64_t entry = root[i];
        if ((entry & PTE_PRESENT) == 0) continue;

        const uint64_t child = entry & PTE_ADDRESS_MASK;
        if (!free_table_tree(child, 3)) ok = false;
        root[i] = 0;
    }

    if (!pmm_free_page(space)) ok = false;
    return ok;
}

bool vmm_switch_address_space(vmm_space_t space) {
    if (!initialized || !valid_space(space)) return false;
    write_cr3(space);
    return true;
}

bool vmm_map_page(vmm_space_t space,
                  uint64_t virtual_address,
                  uint64_t physical_address,
                  uint64_t flags) {
    if (!initialized || !valid_space(space)) return false;
    if (!is_canonical_48(virtual_address)) return false;
    if ((virtual_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
    if ((physical_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
    if ((physical_address & ~PTE_ADDRESS_MASK) != 0) return false;
    if (!flags_supported(flags)) return false;

    /* Non-kernel roots may not mutate the shared upper-half page-table trees. */
    if (space != kernel_root_phys && is_upper_half(virtual_address)) return false;

    struct walk_path path;
    const bool user = (flags & VMM_FLAG_USER) != 0;
    if (!walk_to_pte(space, virtual_address, true, user, &path)) return false;

    if ((*path.pte & PTE_PRESENT) != 0) {
        rollback_created_tables(&path);
        return false;
    }

    /* Effective RW/US permissions require those bits at every ancestor level. */
    if ((flags & VMM_FLAG_WRITE) != 0) {
        *path.pml4e |= PTE_WRITE;
        *path.pdpte |= PTE_WRITE;
        *path.pde |= PTE_WRITE;
    }
    if (user) {
        *path.pml4e |= PTE_USER;
        *path.pdpte |= PTE_USER;
        *path.pde |= PTE_USER;
    }

    *path.pte = physical_address | mapping_bits(flags);

    if (current_space_is(space)) invalidate_page(virtual_address);
    return true;
}

bool vmm_map_range(vmm_space_t space,
                   uint64_t virtual_address,
                   uint64_t physical_address,
                   size_t page_count,
                   uint64_t flags) {
    if (page_count == 0) return false;
    if (page_count > UINT64_MAX / TWILIGHT_PAGE_SIZE) return false;

    const uint64_t bytes = (uint64_t)page_count * TWILIGHT_PAGE_SIZE;
    if (virtual_address > UINT64_MAX - (bytes - TWILIGHT_PAGE_SIZE)) return false;
    if (physical_address > UINT64_MAX - (bytes - TWILIGHT_PAGE_SIZE)) return false;

    size_t mapped = 0;
    for (; mapped < page_count; ++mapped) {
        const uint64_t offset = (uint64_t)mapped * TWILIGHT_PAGE_SIZE;
        if (!vmm_map_page(space,
                          virtual_address + offset,
                          physical_address + offset,
                          flags)) {
            break;
        }
    }

    if (mapped == page_count) return true;

    while (mapped != 0) {
        --mapped;
        const uint64_t offset = (uint64_t)mapped * TWILIGHT_PAGE_SIZE;
        (void)vmm_unmap_page(space, virtual_address + offset, 0);
    }
    return false;
}

bool vmm_unmap_page(vmm_space_t space,
                    uint64_t virtual_address,
                    uint64_t *old_physical_address) {
    if (!initialized || !valid_space(space)) return false;
    if (!is_canonical_48(virtual_address)) return false;
    if ((virtual_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
    if (space != kernel_root_phys && is_upper_half(virtual_address)) return false;

    struct walk_path path;
    if (!walk_to_pte(space, virtual_address, false, false, &path)) return false;
    if ((*path.pte & PTE_PRESENT) == 0) return false;

    const uint64_t physical = *path.pte & PTE_ADDRESS_MASK;
    *path.pte = 0;

    /*
     * Reclaim empty paging structures only when the PMM confirms Twilight owns
     * them. Bootloader-created tables are therefore left alone safely.
     */
    if (try_release_empty_table(path.pde, path.pt, path.pt_phys)) {
        if (try_release_empty_table(path.pdpte, path.pd, path.pd_phys)) {
            (void)try_release_empty_table(path.pml4e, path.pdpt, path.pdpt_phys);
        }
    }

    if (current_space_is(space)) invalidate_page(virtual_address);
    if (old_physical_address != 0) *old_physical_address = physical;
    return true;
}

bool vmm_protect_page(vmm_space_t space,
                      uint64_t virtual_address,
                      uint64_t flags) {
    if (!initialized || !valid_space(space)) return false;
    if (!is_canonical_48(virtual_address)) return false;
    if ((virtual_address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return false;
    if (!flags_supported(flags)) return false;
    if (space != kernel_root_phys && is_upper_half(virtual_address)) return false;

    struct walk_path path;
    const bool user = (flags & VMM_FLAG_USER) != 0;
    if (!walk_to_pte(space, virtual_address, false, user, &path)) return false;
    if ((*path.pte & PTE_PRESENT) == 0) return false;

    const uint64_t old = *path.pte;
    const uint64_t physical = old & PTE_ADDRESS_MASK;
    const uint64_t preserve = old & (PTE_ACCESSED | PTE_DIRTY);

    if ((flags & VMM_FLAG_WRITE) != 0) {
        *path.pml4e |= PTE_WRITE;
        *path.pdpte |= PTE_WRITE;
        *path.pde |= PTE_WRITE;
    }
    if (user) {
        *path.pml4e |= PTE_USER;
        *path.pdpte |= PTE_USER;
        *path.pde |= PTE_USER;
    }

    *path.pte = physical | preserve | mapping_bits(flags);
    if (current_space_is(space)) invalidate_page(virtual_address);
    return true;
}

bool vmm_translate(vmm_space_t space,
                   uint64_t virtual_address,
                   uint64_t *physical_address,
                   uint64_t *flags) {
    if (!initialized || !valid_space(space) || !is_canonical_48(virtual_address)) return false;

    uint64_t *pml4 = table_from_phys(space);
    if (pml4 == 0) return false;

    bool effective_write = true;
    bool effective_user = true;
    bool effective_nx = false;

    uint64_t entry = pml4[table_index(virtual_address, 39)];
    if ((entry & PTE_PRESENT) == 0) return false;
    restrict_effective_permissions(entry, &effective_write, &effective_user, &effective_nx);

    uint64_t *pdpt = table_from_phys(entry & PTE_ADDRESS_MASK);
    if (pdpt == 0) return false;

    entry = pdpt[table_index(virtual_address, 30)];
    if ((entry & PTE_PRESENT) == 0) return false;
    restrict_effective_permissions(entry, &effective_write, &effective_user, &effective_nx);

    if ((entry & PTE_HUGE) != 0) {
        const uint64_t physical = (entry & PAGE_1G_MASK) | (virtual_address & ((1ull << 30) - 1ull));
        if (physical_address != 0) *physical_address = physical;
        if (flags != 0) *flags = decoded_leaf_flags(entry, effective_write, effective_user, effective_nx);
        return true;
    }

    uint64_t *pd = table_from_phys(entry & PTE_ADDRESS_MASK);
    if (pd == 0) return false;

    entry = pd[table_index(virtual_address, 21)];
    if ((entry & PTE_PRESENT) == 0) return false;
    restrict_effective_permissions(entry, &effective_write, &effective_user, &effective_nx);

    if ((entry & PTE_HUGE) != 0) {
        const uint64_t physical = (entry & PAGE_2M_MASK) | (virtual_address & ((1ull << 21) - 1ull));
        if (physical_address != 0) *physical_address = physical;
        if (flags != 0) *flags = decoded_leaf_flags(entry, effective_write, effective_user, effective_nx);
        return true;
    }

    uint64_t *pt = table_from_phys(entry & PTE_ADDRESS_MASK);
    if (pt == 0) return false;

    entry = pt[table_index(virtual_address, 12)];
    if ((entry & PTE_PRESENT) == 0) return false;
    restrict_effective_permissions(entry, &effective_write, &effective_user, &effective_nx);

    const uint64_t physical = (entry & PTE_ADDRESS_MASK) | (virtual_address & (TWILIGHT_PAGE_SIZE - 1ull));
    if (physical_address != 0) *physical_address = physical;
    if (flags != 0) *flags = decoded_leaf_flags(entry, effective_write, effective_user, effective_nx);
    return true;
}

bool vmm_self_test(void) {
    if (!initialized) return false;

    struct pmm_stats before;
    struct pmm_stats after;
    pmm_get_stats(&before);

    bool success = false;
    bool mapped = false;
    uint64_t test_virtual = 0;
    uint64_t data_phys = 0;
    vmm_space_t scratch_space = VMM_INVALID_SPACE;

    if (!find_self_test_virtual_address(&test_virtual)) goto cleanup;

    data_phys = pmm_alloc_page();
    if (data_phys == 0) goto cleanup;

    uint64_t map_flags = VMM_FLAG_WRITE;
    if (nx_available) map_flags |= VMM_FLAG_NO_EXECUTE;

    if (!vmm_map_page(kernel_root_phys, test_virtual, data_phys, map_flags)) goto cleanup;
    mapped = true;

    uint64_t translated = 0;
    uint64_t translated_flags = 0;
    if (!vmm_translate(kernel_root_phys, test_virtual + 0x128ull, &translated, &translated_flags)) goto cleanup;
    if (translated != data_phys + 0x128ull) goto cleanup;
    if ((translated_flags & VMM_FLAG_WRITE) == 0) goto cleanup;
    if (nx_available && (translated_flags & VMM_FLAG_NO_EXECUTE) == 0) goto cleanup;

    volatile uint64_t *via_mapping = (volatile uint64_t *)(uintptr_t)test_virtual;
    volatile uint64_t *via_hhdm = (volatile uint64_t *)pmm_phys_to_virt(data_phys);
    if (via_hhdm == 0) goto cleanup;

    *via_mapping = 0x564d4d2d5457494cull; /* "VMM-TWIL" */
    if (*via_hhdm != 0x564d4d2d5457494cull) goto cleanup;

    *via_hhdm = 0x504147455441424cull; /* "PAGETABL" */
    if (*via_mapping != 0x504147455441424cull) goto cleanup;

    uint64_t protected_flags = nx_available ? VMM_FLAG_NO_EXECUTE : 0;
    if (!vmm_protect_page(kernel_root_phys, test_virtual, protected_flags)) goto cleanup;
    if (!vmm_translate(kernel_root_phys, test_virtual, &translated, &translated_flags)) goto cleanup;
    if ((translated_flags & VMM_FLAG_WRITE) != 0) goto cleanup;
    if (nx_available && (translated_flags & VMM_FLAG_NO_EXECUTE) == 0) goto cleanup;

    uint64_t unmapped_phys = 0;
    if (!vmm_unmap_page(kernel_root_phys, test_virtual, &unmapped_phys)) goto cleanup;
    mapped = false;
    if (unmapped_phys != data_phys) goto cleanup;
    if (vmm_translate(kernel_root_phys, test_virtual, &translated, &translated_flags)) goto cleanup;

    if (!pmm_free_page(data_phys)) goto cleanup;
    data_phys = 0;

    scratch_space = vmm_create_address_space();
    if (scratch_space == VMM_INVALID_SPACE) goto cleanup;

    /* A new process root must see the same shared upper-half kernel mapping. */
    const uint64_t kernel_probe = (uint64_t)(uintptr_t)&vmm_self_test;
    uint64_t kernel_phys = 0;
    uint64_t scratch_phys = 0;
    if (!vmm_translate(kernel_root_phys, kernel_probe, &kernel_phys, 0)) goto cleanup;
    if (!vmm_translate(scratch_space, kernel_probe, &scratch_phys, 0)) goto cleanup;
    if (kernel_phys != scratch_phys) goto cleanup;

    if (!vmm_destroy_address_space(scratch_space)) goto cleanup;
    scratch_space = VMM_INVALID_SPACE;

    success = true;

cleanup:
    if (mapped) (void)vmm_unmap_page(kernel_root_phys, test_virtual, 0);
    if (data_phys != 0) (void)pmm_free_page(data_phys);
    if (scratch_space != VMM_INVALID_SPACE && scratch_space != vmm_current_space()) {
        (void)vmm_destroy_address_space(scratch_space);
    }

    pmm_get_stats(&after);
    return success && after.free_pages == before.free_pages;
}
