#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/gdt.h>
#include <twilight/pmm.h>
#include <twilight/serial.h>
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

static void trace_user(const char *message) {
    serial_write("[serial] ring3: ");
    serial_write(message);
    serial_write("\n");
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

static uint64_t current_rsp(void) {
    uint64_t value;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(value));
    return value;
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
    trace_user("self-test start");
    if (!user_mode_is_available()) {
        trace_user("GDT/VMM unavailable");
        return false;
    }

    const vmm_space_t kernel_space = vmm_kernel_space();
    if (kernel_space == VMM_INVALID_SPACE || vmm_current_space() != kernel_space) {
        trace_user("kernel CR3 precondition failed");
        return false;
    }

    const size_t blob_size = (size_t)(user_probe_blob_end - user_probe_blob_start);
    if (blob_size == 0 || blob_size > TWILIGHT_PAGE_SIZE) {
        trace_user("probe blob size invalid");
        return false;
    }

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
    if (user_space == VMM_INVALID_SPACE) {
        trace_user("address-space creation failed");
        goto cleanup;
    }
    trace_user("separate user address space created");

    uint64_t ignored = 0;
    if (vmm_translate(user_space, USER_TEST_CODE_VA, &ignored, 0)) {
        trace_user("user code VA unexpectedly occupied");
        goto cleanup;
    }
    if (vmm_translate(user_space, USER_TEST_STACK_VA, &ignored, 0)) {
        trace_user("user stack VA unexpectedly occupied");
        goto cleanup;
    }

    code_phys = pmm_alloc_page();
    if (code_phys == 0) {
        trace_user("code-page allocation failed");
        goto cleanup;
    }

    stack_phys = pmm_alloc_page();
    if (stack_phys == 0) {
        trace_user("stack-page allocation failed");
        goto cleanup;
    }
    trace_user("code and stack physical pages allocated");

    /* Copy through the HHDM so the user address space never needs to be active
     * while executable bytes are being installed. */
    void *code_direct = pmm_phys_to_virt(code_phys);
    if (code_direct == 0) {
        trace_user("code HHDM mapping unavailable");
        goto cleanup;
    }
    bytes_copy(code_direct, user_probe_blob_start, blob_size);
    trace_user("userspace probe copied");

    /* Temporarily writable in the inactive user CR3, then enforce W^X. */
    if (!vmm_map_page(user_space,
                      USER_TEST_CODE_VA,
                      code_phys,
                      VMM_FLAG_USER | VMM_FLAG_WRITE)) {
        trace_user("user code mapping failed");
        goto cleanup;
    }
    code_mapped = true;

    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    if (!vmm_map_page(user_space,
                      USER_TEST_STACK_VA,
                      stack_phys,
                      stack_flags)) {
        trace_user("user stack mapping failed");
        goto cleanup;
    }
    stack_mapped = true;
    trace_user("user code and stack mapped");

    if (!vmm_protect_page(user_space, USER_TEST_CODE_VA, VMM_FLAG_USER)) {
        trace_user("W^X code protection failed");
        goto cleanup;
    }

    /* Verify the effective permissions before ever loading the user CR3. */
    uint64_t translated = 0;
    uint64_t flags = 0;
    if (!vmm_translate(user_space, USER_TEST_CODE_VA, &translated, &flags) ||
        translated != code_phys ||
        (flags & VMM_FLAG_USER) == 0 ||
        (flags & VMM_FLAG_WRITE) != 0 ||
        (flags & VMM_FLAG_NO_EXECUTE) != 0) {
        trace_user("user code translation/permissions invalid");
        goto cleanup;
    }

    if (!vmm_translate(user_space, USER_TEST_STACK_VA, &translated, &flags) ||
        translated != stack_phys ||
        (flags & VMM_FLAG_USER) == 0 ||
        (flags & VMM_FLAG_WRITE) == 0) {
        trace_user("user stack translation/permissions invalid");
        goto cleanup;
    }
    if (vmm_nx_supported() && (flags & VMM_FLAG_NO_EXECUTE) == 0) {
        trace_user("user stack unexpectedly executable");
        goto cleanup;
    }
    trace_user("user mappings passed permission preflight");

    /* We currently switch CR3 before calling the assembly transition helper.
     * Prove that both the executing kernel text and current kernel stack are
     * visible through the cloned address space first. This catches bootloader
     * stack placement differences without taking a blind page fault. */
    const uint64_t kernel_rip_probe = (uint64_t)(uintptr_t)&user_mode_self_test;
    const uint64_t kernel_rsp_probe = current_rsp();
    if (!vmm_translate(user_space, kernel_rip_probe, &translated, &flags)) {
        trace_user("preflight failed: kernel text missing from user CR3");
        goto cleanup;
    }
    if (!vmm_translate(user_space, kernel_rsp_probe, &translated, &flags)) {
        trace_user("preflight failed: current kernel stack missing from user CR3");
        goto cleanup;
    }
    if (kernel_rsp_probe >= TWILIGHT_PAGE_SIZE &&
        !vmm_translate(user_space, kernel_rsp_probe - TWILIGHT_PAGE_SIZE, &translated, &flags)) {
        trace_user("preflight failed: lower kernel stack page missing from user CR3");
        goto cleanup;
    }
    trace_user("kernel text/stack visible in user CR3");

    probe_ping_seen = false;
    probe_exit_seen = false;

    trace_user("switching to user CR3");
    if (!vmm_switch_address_space(user_space)) {
        trace_user("CR3 switch rejected");
        goto cleanup;
    }
    returned_to_kernel_space = false;
    trace_user("user CR3 active; entering CPL3 with IRETQ");

    const uint64_t user_stack_top = USER_TEST_STACK_VA + TWILIGHT_PAGE_SIZE - 16ull;
    user_mode_enter(USER_TEST_CODE_VA, user_stack_top);

    trace_user("returned from CPL3 exit trap; restoring kernel CR3");
    /* user_mode_enter() returns in CPL0 but deliberately leaves the test CR3
     * active. Restore the kernel address space before reclaiming user tables. */
    if (!vmm_switch_address_space(kernel_space)) {
        /* Kernel upper-half mappings are still shared, so the caller can panic
         * safely, but destroying the currently active user CR3 would be unsafe. */
        trace_user("FATAL: could not restore kernel CR3");
        return false;
    }
    returned_to_kernel_space = true;

    if (!probe_ping_seen || !probe_exit_seen) {
        trace_user("syscall ping/exit markers were not both observed");
        goto cleanup;
    }
    trace_user("CPL3 syscall round-trip succeeded");
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

    trace_user(success ? "self-test complete: PASS" : "self-test complete: FAIL");
    return success;
}
