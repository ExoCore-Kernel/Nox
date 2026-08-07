#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/gdt.h>
#include <twilight/pmm.h>
#include <twilight/user.h>
#include <twilight/vmm.h>

#define USER_TEST_CODE_VA  0x0000000040000000ull
#define USER_TEST_STACK_VA 0x0000000070000000ull

#define USER_TEST_SYSCALL_PING 0x54570000ull
#define USER_TEST_SYSCALL_EXIT 0x54570001ull

extern const uint8_t user_probe_blob_start[];
extern const uint8_t user_probe_blob_end[];
extern void user_mode_enter(uint64_t instruction_pointer, uint64_t stack_pointer);

static volatile bool probe_ping_seen;
static volatile bool probe_exit_seen;

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

bool user_mode_is_available(void) {
    return gdt_is_initialized() && vmm_is_initialized();
}

int user_syscall_dispatch(uint64_t syscall_number) {
    if (syscall_number == USER_TEST_SYSCALL_PING) {
        probe_ping_seen = true;
        return 0;
    }

    if (syscall_number == USER_TEST_SYSCALL_EXIT) {
        probe_exit_seen = true;
        /* The assembly stub consumes the privilege-change frame and returns
         * directly to the kernel call site instead of IRETQing to ring 3. */
        return 1;
    }

    /* Unknown calls return to user mode. A real syscall ABI comes later. */
    return 0;
}

bool user_mode_self_test(void) {
    if (!user_mode_is_available()) return false;

    const vmm_space_t kernel_space = vmm_kernel_space();
    if (kernel_space == VMM_INVALID_SPACE || vmm_current_space() != kernel_space) return false;

    const size_t blob_size = (size_t)(user_probe_blob_end - user_probe_blob_start);
    if (blob_size == 0 || blob_size > TWILIGHT_PAGE_SIZE) return false;

    struct pmm_stats before;
    struct pmm_stats after;
    pmm_get_stats(&before);

    vmm_space_t user_space = VMM_INVALID_SPACE;
    uint64_t code_phys = 0;
    uint64_t stack_phys = 0;
    bool code_mapped = false;
    bool stack_mapped = false;
    bool returned_to_kernel_space = true;
    bool success = false;

    user_space = vmm_create_address_space();
    if (user_space == VMM_INVALID_SPACE) goto cleanup;

    uint64_t ignored = 0;
    if (vmm_translate(user_space, USER_TEST_CODE_VA, &ignored, 0)) goto cleanup;
    if (vmm_translate(user_space, USER_TEST_STACK_VA, &ignored, 0)) goto cleanup;

    code_phys = pmm_alloc_page();
    if (code_phys == 0) goto cleanup;

    stack_phys = pmm_alloc_page();
    if (stack_phys == 0) goto cleanup;

    /* Copy through the HHDM so the user address space never needs to be active
     * while executable bytes are being installed. */
    void *code_direct = pmm_phys_to_virt(code_phys);
    if (code_direct == 0) goto cleanup;
    bytes_copy(code_direct, user_probe_blob_start, blob_size);

    /* Temporarily writable in the inactive user CR3, then enforce W^X. */
    if (!vmm_map_page(user_space,
                      USER_TEST_CODE_VA,
                      code_phys,
                      VMM_FLAG_USER | VMM_FLAG_WRITE)) {
        goto cleanup;
    }
    code_mapped = true;

    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    if (!vmm_map_page(user_space,
                      USER_TEST_STACK_VA,
                      stack_phys,
                      stack_flags)) {
        goto cleanup;
    }
    stack_mapped = true;

    if (!vmm_protect_page(user_space, USER_TEST_CODE_VA, VMM_FLAG_USER)) goto cleanup;

    probe_ping_seen = false;
    probe_exit_seen = false;

    if (!vmm_switch_address_space(user_space)) goto cleanup;
    returned_to_kernel_space = false;

    const uint64_t user_stack_top = USER_TEST_STACK_VA + TWILIGHT_PAGE_SIZE - 16ull;
    user_mode_enter(USER_TEST_CODE_VA, user_stack_top);

    /* user_mode_enter() returns in CPL0 but deliberately leaves the test CR3
     * active. Restore the kernel address space before reclaiming user tables. */
    if (!vmm_switch_address_space(kernel_space)) {
        /* Kernel upper-half mappings are still shared, so the caller can panic
         * safely, but destroying the currently active user CR3 would be unsafe. */
        return false;
    }
    returned_to_kernel_space = true;

    if (!probe_ping_seen || !probe_exit_seen) goto cleanup;
    success = true;

cleanup:
    if (!returned_to_kernel_space && vmm_current_space() != kernel_space) {
        if (!vmm_switch_address_space(kernel_space)) return false;
    }

    if (user_space != VMM_INVALID_SPACE) {
        if (code_mapped) {
            uint64_t old_phys = 0;
            if (!vmm_unmap_page(user_space, USER_TEST_CODE_VA, &old_phys)) success = false;
            else if (old_phys != code_phys) success = false;
        }
        if (stack_mapped) {
            uint64_t old_phys = 0;
            if (!vmm_unmap_page(user_space, USER_TEST_STACK_VA, &old_phys)) success = false;
            else if (old_phys != stack_phys) success = false;
        }

        if (!vmm_destroy_address_space(user_space)) success = false;
    }

    if (code_phys != 0 && !pmm_free_page(code_phys)) success = false;
    if (stack_phys != 0 && !pmm_free_page(stack_phys)) success = false;

    pmm_get_stats(&after);
    if (after.free_pages != before.free_pages) success = false;

    return success;
}
