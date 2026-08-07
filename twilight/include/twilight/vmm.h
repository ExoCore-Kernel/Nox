#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * An address space is identified by the physical address of its x86_64 PML4.
 * Zero is reserved as the invalid/failure value.
 */
typedef uint64_t vmm_space_t;

#define VMM_INVALID_SPACE 0ull

/* Architecture-neutral mapping properties exposed by Twilight's VMM. */
#define VMM_FLAG_WRITE         (1ull << 0)
#define VMM_FLAG_USER          (1ull << 1)
#define VMM_FLAG_WRITE_THROUGH (1ull << 2)
#define VMM_FLAG_CACHE_DISABLE (1ull << 3)
#define VMM_FLAG_GLOBAL        (1ull << 4)
#define VMM_FLAG_NO_EXECUTE    (1ull << 5)

bool vmm_init(void);
bool vmm_is_initialized(void);
bool vmm_nx_supported(void);

vmm_space_t vmm_kernel_space(void);
vmm_space_t vmm_current_space(void);
vmm_space_t vmm_create_address_space(void);
bool vmm_destroy_address_space(vmm_space_t space);
bool vmm_switch_address_space(vmm_space_t space);

bool vmm_map_page(vmm_space_t space,
                  uint64_t virtual_address,
                  uint64_t physical_address,
                  uint64_t flags);
bool vmm_map_range(vmm_space_t space,
                   uint64_t virtual_address,
                   uint64_t physical_address,
                   size_t page_count,
                   uint64_t flags);
bool vmm_unmap_page(vmm_space_t space,
                    uint64_t virtual_address,
                    uint64_t *old_physical_address);
bool vmm_protect_page(vmm_space_t space,
                      uint64_t virtual_address,
                      uint64_t flags);
bool vmm_translate(vmm_space_t space,
                   uint64_t virtual_address,
                   uint64_t *physical_address,
                   uint64_t *flags);

/* Exercises real page-table creation, TLB invalidation and address spaces. */
bool vmm_self_test(void);
