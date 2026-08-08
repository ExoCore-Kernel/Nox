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

#define IA32_EFER_MSR  0xc0000080u
#define IA32_STAR_MSR  0xc0000081u
#define IA32_LSTAR_MSR 0xc0000082u
#define IA32_FMASK_MSR 0xc0000084u
#define IA32_EFER_SCE   (1ull << 0)

#define LINUX_SYS_WRITE      1ull
#define LINUX_SYS_EXIT       60ull
#define LINUX_SYS_EXIT_GROUP 231ull
#define LINUX_EBADF          9
#define LINUX_EFAULT         14
#define LINUX_ENOSYS         38
#define LINUX_EXIT_SENTINEL  ((int64_t)INT64_MIN)

#define ELF_NIDENT 16u
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PF_X 0x1u
#define PF_W 0x2u

#define LINUX_USER_STACK_VA 0x00007fffffffe000ull
#define LINUX_USER_MAX_PAGES 64u
#define LINUX_USER_TOP_LIMIT 0x0000800000000000ull

extern const uint8_t user_probe_blob_start[];
extern const uint8_t user_probe_blob_end[];
extern void user_mode_enter(uint64_t instruction_pointer,
                            uint64_t stack_pointer,
                            vmm_space_t user_space,
                            vmm_space_t kernel_space);
extern void linux_syscall_entry(void);

#if TWILIGHT_LINUX_USER_SELF_TEST
extern const uint8_t twilight_linux_hello_elf[];
extern const size_t twilight_linux_hello_elf_size;
#endif

struct __attribute__((packed)) elf64_ehdr {
    uint8_t ident[ELF_NIDENT];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

struct __attribute__((packed)) elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
};

struct linux_user_page {
    uint64_t va;
    uint64_t phys;
    uint64_t final_flags;
};

struct linux_user_image {
    vmm_space_t space;
    uint64_t entry;
    uint64_t stack_pointer;
    struct linux_user_page pages[LINUX_USER_MAX_PAGES];
    size_t page_count;
};

static volatile bool probe_ping_seen;
static volatile bool probe_exit_seen;
static volatile bool linux_write_seen;
static volatile bool linux_exit_seen;
static volatile int linux_exit_status;
static vmm_space_t linux_active_space;

static void trace_user(const char *message) {
    serial_write("[serial] ring3: ");
    serial_write(message);
    serial_write("\n");
}

static void trace_linux_user(const char *message) {
    serial_write("[serial] linux-user: ");
    serial_write(message);
    serial_write("\n");
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

static void bytes_zero(void *destination, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < size; ++i) out[i] = 0;
}

static uint64_t align_down_page(uint64_t value) {
    return value & ~(TWILIGHT_PAGE_SIZE - 1ull);
}

static bool align_up_page(uint64_t value, uint64_t *out) {
    if (out == 0 || value > UINT64_MAX - (TWILIGHT_PAGE_SIZE - 1ull)) return false;
    *out = (value + TWILIGHT_PAGE_SIZE - 1ull) & ~(TWILIGHT_PAGE_SIZE - 1ull);
    return true;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(leaf), "c"(subleaf));
    if (a != 0) *a = eax;
    if (b != 0) *b = ebx;
    if (c != 0) *c = ecx;
    if (d != 0) *d = edx;
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile ("wrmsr" ::
                      "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                      : "memory");
}

static bool linux_syscall_cpu_supported(void) {
    uint32_t max_extended = 0;
    cpuid(0x80000000u, 0, &max_extended, 0, 0, 0);
    if (max_extended < 0x80000001u) return false;
    uint32_t edx = 0;
    cpuid(0x80000001u, 0, 0, 0, 0, &edx);
    return (edx & (1u << 11)) != 0; /* SYSCALL/SYSRET */
}

static bool linux_syscall_init(void) {
    if (!linux_syscall_cpu_supported()) return false;

    uint64_t efer = read_msr(IA32_EFER_MSR);
    efer |= IA32_EFER_SCE;
    write_msr(IA32_EFER_MSR, efer);

    /* SYSCALL loads CS=0x08 and SS=0x10. SYSRETQ derives user selectors as
     * STAR.user_base+16=0x23 (code) and +8=0x1b (data), so base=0x13. */
    const uint64_t star = ((uint64_t)0x13u << 48) |
                          ((uint64_t)TWILIGHT_GDT_KERNEL_CODE << 32);
    write_msr(IA32_STAR_MSR, star);
    write_msr(IA32_LSTAR_MSR, (uint64_t)(uintptr_t)&linux_syscall_entry);

    /* Clear IF and DF while inside the early syscall path. The user's original
     * flags are preserved by the CPU in R11 and restored by SYSRETQ. */
    write_msr(IA32_FMASK_MSR, (1ull << 9) | (1ull << 10));

    return (read_msr(IA32_EFER_MSR) & IA32_EFER_SCE) != 0 &&
           read_msr(IA32_STAR_MSR) == star &&
           read_msr(IA32_LSTAR_MSR) == (uint64_t)(uintptr_t)&linux_syscall_entry;
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
        /* The assembly stub restores the kernel CR3 and kernel stack instead
         * of IRETQing back to CPL3 for this test-only exit call. */
        return 1;
    }

    /* Unknown calls return to user mode. */
    return 0;
}

static bool linux_user_range_readable(uint64_t address, uint64_t length) {
    if (linux_active_space == VMM_INVALID_SPACE) return false;
    if (length == 0) return true;
    if (address >= LINUX_USER_TOP_LIMIT || length > LINUX_USER_TOP_LIMIT ||
        address > UINT64_MAX - length) return false;
    const uint64_t end = address + length;
    if (end > LINUX_USER_TOP_LIMIT) return false;

    uint64_t page = align_down_page(address);
    while (page < end) {
        uint64_t phys = 0;
        uint64_t flags = 0;
        if (!vmm_translate(linux_active_space, page, &phys, &flags) ||
            (flags & VMM_FLAG_USER) == 0) return false;
        if (page > UINT64_MAX - TWILIGHT_PAGE_SIZE) return false;
        page += TWILIGHT_PAGE_SIZE;
    }
    return true;
}

int64_t linux_syscall_dispatch(uint64_t syscall_number,
                               uint64_t arg1,
                               uint64_t arg2,
                               uint64_t arg3,
                               uint64_t arg4,
                               uint64_t arg5,
                               uint64_t arg6) {
    (void)arg4;
    (void)arg5;
    (void)arg6;

    if (syscall_number == LINUX_SYS_WRITE) {
        const int fd = (int)arg1;
        const uint64_t address = arg2;
        const uint64_t length = arg3;
        if (fd != 1 && fd != 2) return -LINUX_EBADF;
        if (!linux_user_range_readable(address, length)) return -LINUX_EFAULT;

        const char *bytes = (const char *)(uintptr_t)address;
        serial_write(fd == 1 ? "[linux:user stdout] " : "[linux:user stderr] ");
        for (uint64_t i = 0; i < length; ++i) serial_write_char(bytes[i]);
        if (length == 0 || bytes[length - 1u] != '\n') serial_write_char('\n');
        linux_write_seen = true;
        return (int64_t)length;
    }

    if (syscall_number == LINUX_SYS_EXIT || syscall_number == LINUX_SYS_EXIT_GROUP) {
        linux_exit_status = (int)(arg1 & 0xffu);
        linux_exit_seen = true;
        return LINUX_EXIT_SENTINEL;
    }

    return -LINUX_ENOSYS;
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
    bool success = false;

    user_space = vmm_create_address_space();
    if (user_space == VMM_INVALID_SPACE) {
        trace_user("address-space creation failed");
        goto cleanup;
    }
    trace_user("separate user address space created");

    uint64_t translated = 0;
    uint64_t flags = 0;
    if (vmm_translate(user_space, USER_TEST_CODE_VA, &translated, 0)) {
        trace_user("user code VA unexpectedly occupied");
        goto cleanup;
    }
    if (vmm_translate(user_space, USER_TEST_STACK_VA, &translated, 0)) {
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

    /* Copy through the HHDM while the kernel CR3 is active. */
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

    /* Verify the effective permissions before the assembly ever loads CR3. */
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

    /* Kernel text must be shared into every process CR3 because the CPU enters
     * the Ring-0 syscall/exception stubs before Twilight can switch back to the
     * kernel address space. */
    if (!vmm_translate(user_space,
                       (uint64_t)(uintptr_t)&user_mode_enter,
                       &translated,
                       &flags)) {
        trace_user("preflight failed: transition code missing from user CR3");
        goto cleanup;
    }
    if (!vmm_translate(user_space,
                       (uint64_t)(uintptr_t)&user_syscall_dispatch,
                       &translated,
                       &flags)) {
        trace_user("preflight failed: syscall dispatcher missing from user CR3");
        goto cleanup;
    }
    trace_user("user mappings and shared kernel transition code passed preflight");

    probe_ping_seen = false;
    probe_exit_seen = false;

    /* The assembly helper owns the CR3 switch and uses a dedicated upper-half
     * kernel transition stack. C never executes on the user CR3. */
    const uint64_t user_stack_top = USER_TEST_STACK_VA + TWILIGHT_PAGE_SIZE - 16ull;
    trace_user("entering assembly CR3/CPL3 transition");
    user_mode_enter(USER_TEST_CODE_VA,
                    user_stack_top,
                    user_space,
                    kernel_space);
    trace_user("returned to kernel CR3 from CPL3 exit trap");

    if (vmm_current_space() != kernel_space) {
        trace_user("FATAL: assembly returned with wrong CR3");
        return false;
    }

    if (!probe_ping_seen || !probe_exit_seen) {
        trace_user("syscall ping/exit markers were not both observed");
        goto cleanup;
    }
    trace_user("CPL3 syscall round-trip succeeded");
    success = true;

cleanup:
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

#if TWILIGHT_LINUX_USER_SELF_TEST
static struct linux_user_page *linux_image_find_page(struct linux_user_image *image,
                                                     uint64_t va) {
    if (image == 0) return 0;
    for (size_t i = 0; i < image->page_count; ++i) {
        if (image->pages[i].va == va) return &image->pages[i];
    }
    return 0;
}

static struct linux_user_page *linux_image_add_page(struct linux_user_image *image,
                                                    uint64_t va,
                                                    uint64_t final_flags) {
    if (image == 0 || image->space == VMM_INVALID_SPACE ||
        (va & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return 0;

    struct linux_user_page *existing = linux_image_find_page(image, va);
    if (existing != 0) {
        existing->final_flags |= final_flags & VMM_FLAG_WRITE;
        if ((final_flags & VMM_FLAG_NO_EXECUTE) == 0)
            existing->final_flags &= ~VMM_FLAG_NO_EXECUTE;
        return existing;
    }

    if (image->page_count >= LINUX_USER_MAX_PAGES) return 0;
    uint64_t phys = pmm_alloc_page();
    if (phys == 0) return 0;
    void *direct = pmm_phys_to_virt(phys);
    if (direct == 0) {
        (void)pmm_free_page(phys);
        return 0;
    }
    bytes_zero(direct, TWILIGHT_PAGE_SIZE);

    if (!vmm_map_page(image->space, va, phys, VMM_FLAG_USER | VMM_FLAG_WRITE)) {
        (void)pmm_free_page(phys);
        return 0;
    }

    struct linux_user_page *page = &image->pages[image->page_count++];
    page->va = va;
    page->phys = phys;
    page->final_flags = final_flags | VMM_FLAG_USER;
    return page;
}

static bool linux_image_copy_to_user(struct linux_user_image *image,
                                     uint64_t destination,
                                     const uint8_t *source,
                                     uint64_t length) {
    if (image == 0 || source == 0) return false;
    for (uint64_t i = 0; i < length; ++i) {
        const uint64_t va = destination + i;
        struct linux_user_page *page = linux_image_find_page(image, align_down_page(va));
        if (page == 0) return false;
        uint8_t *direct = (uint8_t *)pmm_phys_to_virt(page->phys);
        if (direct == 0) return false;
        direct[va & (TWILIGHT_PAGE_SIZE - 1ull)] = source[i];
    }
    return true;
}

static bool linux_image_write_u64(struct linux_user_image *image,
                                  uint64_t destination,
                                  uint64_t value) {
    uint8_t bytes[8];
    for (unsigned int i = 0; i < 8u; ++i) bytes[i] = (uint8_t)(value >> (i * 8u));
    return linux_image_copy_to_user(image, destination, bytes, sizeof(bytes));
}

static bool linux_image_finalize_permissions(struct linux_user_image *image) {
    if (image == 0) return false;
    for (size_t i = 0; i < image->page_count; ++i) {
        const uint64_t flags = image->pages[i].final_flags;
        /* Keep W^X as a hard loader invariant. */
        if ((flags & VMM_FLAG_WRITE) != 0 && (flags & VMM_FLAG_NO_EXECUTE) == 0)
            return false;
        if (!vmm_protect_page(image->space, image->pages[i].va, flags)) return false;
    }
    return true;
}

static void linux_image_destroy(struct linux_user_image *image) {
    if (image == 0) return;
    if (image->space != VMM_INVALID_SPACE) {
        for (size_t i = 0; i < image->page_count; ++i) {
            uint64_t old_phys = 0;
            (void)vmm_unmap_page(image->space, image->pages[i].va, &old_phys);
        }
        (void)vmm_destroy_address_space(image->space);
    }
    for (size_t i = 0; i < image->page_count; ++i) {
        if (image->pages[i].phys != 0) (void)pmm_free_page(image->pages[i].phys);
    }
    *image = (struct linux_user_image){0};
}

static bool linux_elf_load(const uint8_t *elf, size_t elf_size,
                           struct linux_user_image *image) {
    if (elf == 0 || image == 0 || elf_size < sizeof(struct elf64_ehdr)) return false;
    *image = (struct linux_user_image){0};

    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)elf;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != ELFCLASS64 ||
        eh->ident[5] != ELFDATA2LSB || eh->ident[6] != EV_CURRENT ||
        eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->version != EV_CURRENT || eh->ehsize != sizeof(struct elf64_ehdr) ||
        eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0) return false;

    if (eh->phoff > elf_size ||
        (uint64_t)eh->phnum > (UINT64_MAX - eh->phoff) / sizeof(struct elf64_phdr) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > elf_size)
        return false;

    image->space = vmm_create_address_space();
    if (image->space == VMM_INVALID_SPACE) return false;

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)(elf + (size_t)eh->phoff);

    bool entry_covered = false;
    for (uint16_t index = 0; index < eh->phnum; ++index) {
        const struct elf64_phdr *ph = &phdrs[index];
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset > elf_size ||
            ph->filesz > (uint64_t)elf_size - ph->offset ||
            ph->vaddr >= LINUX_USER_TOP_LIMIT ||
            ph->memsz > LINUX_USER_TOP_LIMIT - ph->vaddr) {
            linux_image_destroy(image);
            return false;
        }

        const uint64_t segment_end = ph->vaddr + ph->memsz;
        if (eh->entry >= ph->vaddr && eh->entry < segment_end && (ph->flags & PF_X) != 0)
            entry_covered = true;

        uint64_t page_end = 0;
        if (!align_up_page(segment_end, &page_end)) {
            linux_image_destroy(image);
            return false;
        }
        uint64_t final_flags = VMM_FLAG_USER;
        if ((ph->flags & PF_W) != 0) final_flags |= VMM_FLAG_WRITE;
        if ((ph->flags & PF_X) == 0 && vmm_nx_supported()) final_flags |= VMM_FLAG_NO_EXECUTE;

        for (uint64_t page = align_down_page(ph->vaddr); page < page_end;
             page += TWILIGHT_PAGE_SIZE) {
            if (linux_image_add_page(image, page, final_flags) == 0) {
                linux_image_destroy(image);
                return false;
            }
        }

        if (ph->filesz != 0 &&
            !linux_image_copy_to_user(image, ph->vaddr,
                                      elf + (size_t)ph->offset, ph->filesz)) {
            linux_image_destroy(image);
            return false;
        }
    }

    if (!entry_covered) {
        linux_image_destroy(image);
        return false;
    }

    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    if (linux_image_add_page(image, LINUX_USER_STACK_VA, stack_flags) == 0) {
        linux_image_destroy(image);
        return false;
    }

    const char argv0[] = "/bin/hello";
    const uint64_t stack_top = LINUX_USER_STACK_VA + TWILIGHT_PAGE_SIZE;
    const uint64_t argv0_va = stack_top - 32ull;
    if (!linux_image_copy_to_user(image, argv0_va,
                                  (const uint8_t *)argv0, sizeof(argv0))) {
        linux_image_destroy(image);
        return false;
    }

    /* argc, argv[0], argv NULL, envp NULL, AT_NULL, 0. */
    uint64_t sp = (argv0_va - 48ull) & ~15ull;
    if (!linux_image_write_u64(image, sp + 0, 1) ||
        !linux_image_write_u64(image, sp + 8, argv0_va) ||
        !linux_image_write_u64(image, sp + 16, 0) ||
        !linux_image_write_u64(image, sp + 24, 0) ||
        !linux_image_write_u64(image, sp + 32, 0) ||
        !linux_image_write_u64(image, sp + 40, 0)) {
        linux_image_destroy(image);
        return false;
    }

    if (!linux_image_finalize_permissions(image)) {
        linux_image_destroy(image);
        return false;
    }

    image->entry = eh->entry;
    image->stack_pointer = sp;
    return true;
}

bool linux_user_self_test(void) {
    trace_linux_user("ELF64 + native SYSCALL self-test start");
    if (!user_mode_is_available()) {
        trace_linux_user("Ring3/VMM unavailable");
        return false;
    }
    if (!linux_syscall_init()) {
        trace_linux_user("CPU SYSCALL/SYSRET or MSR setup unavailable");
        return false;
    }

    const vmm_space_t kernel_space = vmm_kernel_space();
    if (kernel_space == VMM_INVALID_SPACE || vmm_current_space() != kernel_space) {
        trace_linux_user("kernel CR3 precondition failed");
        return false;
    }

    struct pmm_stats before;
    struct pmm_stats after;
    pmm_get_stats(&before);

    struct linux_user_image image;
    if (!linux_elf_load(twilight_linux_hello_elf,
                        twilight_linux_hello_elf_size,
                        &image)) {
        trace_linux_user("ELF loader rejected test executable");
        return false;
    }
    trace_linux_user("ELF64 PT_LOAD segments mapped with W^X protections");

    uint64_t translated = 0;
    uint64_t flags = 0;
    if (!vmm_translate(image.space,
                       (uint64_t)(uintptr_t)&linux_syscall_entry,
                       &translated, &flags)) {
        trace_linux_user("LSTAR entry is not shared into process CR3");
        linux_image_destroy(&image);
        return false;
    }

    linux_active_space = image.space;
    linux_write_seen = false;
    linux_exit_seen = false;
    linux_exit_status = -1;

    trace_linux_user("entering linked Linux ELF at CPL3");
    user_mode_enter(image.entry, image.stack_pointer, image.space, kernel_space);
    trace_linux_user("Linux exit_group returned control to Twilight kernel");

    linux_active_space = VMM_INVALID_SPACE;

    bool success = vmm_current_space() == kernel_space &&
                   linux_write_seen && linux_exit_seen && linux_exit_status == 0;
    if (!success) trace_linux_user("Linux syscall/exit observations incomplete");

    linux_image_destroy(&image);
    pmm_get_stats(&after);
    if (after.free_pages != before.free_pages) success = false;

    trace_linux_user(success ?
        "self-test complete: PASS (real x86-64 Linux ELF executed write + exit_group via SYSCALL)" :
        "self-test complete: FAIL");
    return success;
}
#else
bool linux_user_self_test(void) {
    return true;
}
#endif
