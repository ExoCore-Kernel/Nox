#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/module.h>
#include <twilight/gdt.h>
#include <twilight/pmm.h>
#include <twilight/serial.h>
#include <twilight/vmm.h>

#if TWILIGHT_BUSYBOX_SELF_TEST

#define IA32_EFER_MSR    0xc0000080u
#define IA32_STAR_MSR    0xc0000081u
#define IA32_LSTAR_MSR   0xc0000082u
#define IA32_FMASK_MSR   0xc0000084u
#define IA32_FS_BASE_MSR 0xc0000100u
#define IA32_EFER_SCE     (1ull << 0)

#define LINUX_EXIT_SENTINEL ((int64_t)INT64_MIN)

#define SYS_READ             0ull
#define SYS_WRITE            1ull
#define SYS_CLOSE            3ull
#define SYS_FSTAT            5ull
#define SYS_LSEEK            8ull
#define SYS_MMAP             9ull
#define SYS_MPROTECT        10ull
#define SYS_MUNMAP          11ull
#define SYS_BRK             12ull
#define SYS_RT_SIGACTION    13ull
#define SYS_RT_SIGPROCMASK  14ull
#define SYS_IOCTL           16ull
#define SYS_WRITEV          20ull
#define SYS_ACCESS          21ull
#define SYS_MADVISE         28ull
#define SYS_GETPID          39ull
#define SYS_UNAME           63ull
#define SYS_FCNTL           72ull
#define SYS_GETCWD          79ull
#define SYS_READLINK        89ull
#define SYS_GETUID         102ull
#define SYS_GETGID         104ull
#define SYS_GETEUID        107ull
#define SYS_GETEGID        108ull
#define SYS_GETPPID        110ull
#define SYS_ARCH_PRCTL     158ull
#define SYS_GETTID         186ull
#define SYS_FUTEX          202ull
#define SYS_SET_TID_ADDRESS 218ull
#define SYS_CLOCK_GETTIME  228ull
#define SYS_EXIT_GROUP     231ull
#define SYS_OPENAT         257ull
#define SYS_NEWFSTATAT     262ull
#define SYS_SET_ROBUST_LIST 273ull
#define SYS_PRLIMIT64      302ull
#define SYS_GETRANDOM      318ull
#define SYS_RSEQ           334ull
#define SYS_EXIT            60ull

#define LINUX_EPERM       1
#define LINUX_ENOENT      2
#define LINUX_EBADF       9
#define LINUX_EAGAIN     11
#define LINUX_ENOMEM     12
#define LINUX_EACCES     13
#define LINUX_EFAULT     14
#define LINUX_EINVAL     22
#define LINUX_ENOTTY     25
#define LINUX_ESPIPE     29
#define LINUX_ENOSYS     38

#define ARCH_SET_GS 0x1001ull
#define ARCH_SET_FS 0x1002ull
#define ARCH_GET_FS 0x1003ull
#define ARCH_GET_GS 0x1004ull

#define PROT_READ  0x1ull
#define PROT_WRITE 0x2ull
#define PROT_EXEC  0x4ull
#define MAP_PRIVATE   0x02ull
#define MAP_FIXED     0x10ull
#define MAP_ANONYMOUS 0x20ull

#define ELF_NIDENT 16u
#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EV_CURRENT 1u
#define ET_EXEC 2u
#define EM_X86_64 62u
#define PT_LOAD 1u
#define PT_PHDR 6u
#define PF_X 0x1u
#define PF_W 0x2u

#define AT_NULL   0ull
#define AT_PHDR   3ull
#define AT_PHENT  4ull
#define AT_PHNUM  5ull
#define AT_PAGESZ 6ull
#define AT_BASE   7ull
#define AT_FLAGS  8ull
#define AT_ENTRY  9ull
#define AT_UID   11ull
#define AT_EUID  12ull
#define AT_GID   13ull
#define AT_EGID  14ull
#define AT_PLATFORM 15ull
#define AT_HWCAP 16ull
#define AT_CLKTCK 17ull
#define AT_SECURE 23ull
#define AT_RANDOM 25ull
#define AT_HWCAP2 26ull
#define AT_EXECFN 31ull

#define BUSYBOX_USER_TOP      0x0000800000000000ull
#define BUSYBOX_STACK_TOP     0x00007fffffffe000ull
#define BUSYBOX_STACK_PAGES   32u
#define BUSYBOX_MAX_PAGES     768u
#define BUSYBOX_MMAP_BASE     0x0000002000000000ull
#define BUSYBOX_MMAP_LIMIT    0x0000003000000000ull

extern const uint8_t twilight_busybox_elf[];
extern const size_t twilight_busybox_elf_size;
extern void user_mode_enter(uint64_t instruction_pointer,
                            uint64_t stack_pointer,
                            vmm_space_t user_space,
                            vmm_space_t kernel_space);
extern void linux_syscall_entry(void);

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

struct busybox_page {
    uint64_t va;
    uint64_t phys;
    uint64_t flags;
};

struct busybox_image {
    vmm_space_t space;
    uint64_t entry;
    uint64_t stack_pointer;
    uint64_t phdr;
    uint16_t phnum;
    uint16_t phentsize;
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t mmap_next;
    struct busybox_page pages[BUSYBOX_MAX_PAGES];
    size_t page_count;
};

struct linux_iovec {
    uint64_t base;
    uint64_t len;
};

struct aux_pair {
    uint64_t type;
    uint64_t value;
};

static struct busybox_image image;
static bool process_active;
static bool write_seen;
static bool exit_seen;
static bool unknown_syscall_seen;
static int exit_status;
static uint64_t fs_base;
static uint64_t random_state = 0x4e4f584255535942ull;

static void trace(const char *text) {
    serial_write("[serial] busybox: ");
    serial_write(text);
    serial_write("\n");
}

static void bytes_zero(void *pointer, size_t size) {
    uint8_t *out = pointer;
    for (size_t i = 0; i < size; ++i) out[i] = 0;
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

static size_t string_length(const char *text) {
    size_t length = 0;
    if (text == 0) return 0;
    while (text[length] != '\0') ++length;
    return length;
}

static uint64_t align_down(uint64_t value) {
    return value & ~(TWILIGHT_PAGE_SIZE - 1ull);
}

static bool align_up(uint64_t value, uint64_t *out) {
    if (out == 0 || value > UINT64_MAX - (TWILIGHT_PAGE_SIZE - 1ull)) return false;
    *out = (value + TWILIGHT_PAGE_SIZE - 1ull) & ~(TWILIGHT_PAGE_SIZE - 1ull);
    return true;
}

static void serial_u64(uint64_t value) {
    char reverse[32];
    size_t count = 0;
    do {
        reverse[count++] = (char)('0' + value % 10ull);
        value /= 10ull;
    } while (value != 0 && count < sizeof(reverse));
    while (count != 0) serial_write_char(reverse[--count]);
}

static void log_unknown_syscall(uint64_t number) {
    serial_write("[linux:busybox] unsupported syscall ");
    serial_u64(number);
    serial_write(" -> -ENOSYS\n");
    unknown_syscall_seen = true;
}

static void cpuid(uint32_t leaf, uint32_t subleaf,
                  uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid"
                      : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                      : "a"(leaf), "c"(subleaf));
    if (a != 0) *a = eax;
    if (b != 0) *b = ebx;
    if (c != 0) *c = ecx;
    if (d != 0) *d = edx;
}

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value) {
    __asm__ volatile ("wrmsr" ::
                      "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32))
                      : "memory");
}

static bool syscall_init(void) {
    uint32_t max_extended = 0;
    cpuid(0x80000000u, 0, &max_extended, 0, 0, 0);
    if (max_extended < 0x80000001u) return false;
    uint32_t edx = 0;
    cpuid(0x80000001u, 0, 0, 0, 0, &edx);
    if ((edx & (1u << 11)) == 0) return false;

    write_msr(IA32_EFER_MSR, read_msr(IA32_EFER_MSR) | IA32_EFER_SCE);
    const uint64_t star = ((uint64_t)0x13u << 48) |
                          ((uint64_t)TWILIGHT_GDT_KERNEL_CODE << 32);
    write_msr(IA32_STAR_MSR, star);
    write_msr(IA32_LSTAR_MSR, (uint64_t)(uintptr_t)&linux_syscall_entry);
    write_msr(IA32_FMASK_MSR, (1ull << 9) | (1ull << 10));
    return (read_msr(IA32_EFER_MSR) & IA32_EFER_SCE) != 0 &&
           read_msr(IA32_LSTAR_MSR) == (uint64_t)(uintptr_t)&linux_syscall_entry;
}

static struct busybox_page *find_page(uint64_t va) {
    const uint64_t page_va = align_down(va);
    for (size_t i = 0; i < image.page_count; ++i) {
        if (image.pages[i].va == page_va) return &image.pages[i];
    }
    return 0;
}

static struct busybox_page *add_page(uint64_t va, uint64_t final_flags) {
    if (image.space == VMM_INVALID_SPACE ||
        (va & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || va >= BUSYBOX_USER_TOP)
        return 0;

    struct busybox_page *existing = find_page(va);
    if (existing != 0) {
        existing->flags |= final_flags & VMM_FLAG_WRITE;
        if ((final_flags & VMM_FLAG_NO_EXECUTE) == 0)
            existing->flags &= ~VMM_FLAG_NO_EXECUTE;
        return existing;
    }

    if (image.page_count >= BUSYBOX_MAX_PAGES) return 0;
    const uint64_t phys = pmm_alloc_page();
    if (phys == 0) return 0;
    void *direct = pmm_phys_to_virt(phys);
    if (direct == 0) {
        (void)pmm_free_page(phys);
        return 0;
    }
    bytes_zero(direct, TWILIGHT_PAGE_SIZE);
    if (!vmm_map_page(image.space, va, phys, VMM_FLAG_USER | VMM_FLAG_WRITE)) {
        (void)pmm_free_page(phys);
        return 0;
    }

    struct busybox_page *page = &image.pages[image.page_count++];
    page->va = va;
    page->phys = phys;
    page->flags = final_flags | VMM_FLAG_USER;
    return page;
}

static bool copy_to_user(uint64_t destination, const void *source, uint64_t length) {
    const uint8_t *input = source;
    while (length != 0) {
        struct busybox_page *page = find_page(destination);
        if (page == 0) return false;
        uint8_t *direct = pmm_phys_to_virt(page->phys);
        if (direct == 0) return false;
        const uint64_t offset = destination & (TWILIGHT_PAGE_SIZE - 1ull);
        uint64_t chunk = TWILIGHT_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        bytes_copy(direct + offset, input, (size_t)chunk);
        destination += chunk;
        input += chunk;
        length -= chunk;
    }
    return true;
}

static bool write_u64(uint64_t destination, uint64_t value) {
    uint8_t bytes[8];
    for (unsigned int i = 0; i < 8u; ++i) bytes[i] = (uint8_t)(value >> (8u * i));
    return copy_to_user(destination, bytes, sizeof(bytes));
}

static bool user_range(uint64_t address, uint64_t length, bool writable) {
    if (!process_active || image.space == VMM_INVALID_SPACE) return false;
    if (length == 0) return true;
    if (address >= BUSYBOX_USER_TOP || address > UINT64_MAX - length ||
        address + length > BUSYBOX_USER_TOP) return false;
    uint64_t page = align_down(address);
    const uint64_t end = address + length;
    while (page < end) {
        uint64_t phys = 0, flags = 0;
        if (!vmm_translate(image.space, page, &phys, &flags) ||
            (flags & VMM_FLAG_USER) == 0 ||
            (writable && (flags & VMM_FLAG_WRITE) == 0)) return false;
        page += TWILIGHT_PAGE_SIZE;
    }
    return true;
}

static bool zero_user(uint64_t address, uint64_t length) {
    if (!user_range(address, length, true)) return false;
    for (uint64_t i = 0; i < length; ++i)
        *(volatile uint8_t *)(uintptr_t)(address + i) = 0;
    return true;
}

static bool user_store_u32(uint64_t address, uint32_t value) {
    if (!user_range(address, 4, true)) return false;
    for (unsigned int i = 0; i < 4u; ++i)
        *(volatile uint8_t *)(uintptr_t)(address + i) = (uint8_t)(value >> (8u * i));
    return true;
}

static bool user_store_u64(uint64_t address, uint64_t value) {
    if (!user_range(address, 8, true)) return false;
    for (unsigned int i = 0; i < 8u; ++i)
        *(volatile uint8_t *)(uintptr_t)(address + i) = (uint8_t)(value >> (8u * i));
    return true;
}

static bool protect_new_page(struct busybox_page *page) {
    if (page == 0) return false;
    if ((page->flags & VMM_FLAG_WRITE) != 0 &&
        (page->flags & VMM_FLAG_NO_EXECUTE) == 0) return false;
    return vmm_protect_page(image.space, page->va, page->flags);
}

static void destroy_image(void) {
    write_msr(IA32_FS_BASE_MSR, 0);
    fs_base = 0;
    if (image.space != VMM_INVALID_SPACE) {
        for (size_t i = 0; i < image.page_count; ++i) {
            uint64_t ignored = 0;
            (void)vmm_unmap_page(image.space, image.pages[i].va, &ignored);
        }
        (void)vmm_destroy_address_space(image.space);
    }
    for (size_t i = 0; i < image.page_count; ++i)
        if (image.pages[i].phys != 0) (void)pmm_free_page(image.pages[i].phys);
    image = (struct busybox_image){0};
}

static bool push_stack_bytes(uint64_t *cursor, const void *data, size_t length,
                             uint64_t *user_address) {
    if (cursor == 0 || data == 0 || length == 0 || *cursor < length) return false;
    *cursor -= (uint64_t)length;
    if (!copy_to_user(*cursor, data, length)) return false;
    if (user_address != 0) *user_address = *cursor;
    return true;
}

static bool push_stack_string(uint64_t *cursor, const char *text, uint64_t *user_address) {
    return push_stack_bytes(cursor, text, string_length(text) + 1u, user_address);
}

static bool build_initial_stack(const struct elf64_ehdr *eh) {
    const uint64_t stack_base = BUSYBOX_STACK_TOP -
                                (uint64_t)BUSYBOX_STACK_PAGES * TWILIGHT_PAGE_SIZE;
    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = stack_base; va < BUSYBOX_STACK_TOP; va += TWILIGHT_PAGE_SIZE) {
        if (add_page(va, stack_flags) == 0) return false;
    }

    uint64_t cursor = BUSYBOX_STACK_TOP;
    const char argv0[] = "/bin/echo";
    const char argv1[] = "Hello from official unmodified BusyBox running on Twilight!";
    const char env0[] = "PATH=/bin:/usr/bin";
    const char platform[] = "x86_64";
    const uint8_t random_bytes[16] = {
        0x54,0x77,0x69,0x6c,0x69,0x67,0x68,0x74,
        0x4e,0x6f,0x78,0x42,0x75,0x73,0x79,0x21,
    };

    uint64_t argv0_va = 0, argv1_va = 0, env0_va = 0, platform_va = 0, random_va = 0;
    if (!push_stack_bytes(&cursor, random_bytes, sizeof(random_bytes), &random_va) ||
        !push_stack_string(&cursor, platform, &platform_va) ||
        !push_stack_string(&cursor, env0, &env0_va) ||
        !push_stack_string(&cursor, argv1, &argv1_va) ||
        !push_stack_string(&cursor, argv0, &argv0_va)) return false;

    const struct aux_pair aux[] = {
        { AT_PHDR, image.phdr },
        { AT_PHENT, image.phentsize },
        { AT_PHNUM, image.phnum },
        { AT_PAGESZ, TWILIGHT_PAGE_SIZE },
        { AT_BASE, 0 },
        { AT_FLAGS, 0 },
        { AT_ENTRY, eh->entry },
        { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_PLATFORM, platform_va },
        { AT_HWCAP, 0 },
        { AT_CLKTCK, 100 },
        { AT_SECURE, 0 },
        { AT_RANDOM, random_va },
        { AT_HWCAP2, 0 },
        { AT_EXECFN, argv0_va },
        { AT_NULL, 0 },
    };

    const size_t aux_words = sizeof(aux) / sizeof(aux[0]) * 2u;
    const size_t words = 1u + 3u + 2u + aux_words; /* argc + argv + envp + auxv */
    const uint64_t table_bytes = (uint64_t)words * 8ull;
    if (cursor < stack_base + table_bytes) return false;
    uint64_t sp = (cursor - table_bytes) & ~15ull;
    uint64_t p = sp;

    if (!write_u64(p, 2)) return false; p += 8;
    if (!write_u64(p, argv0_va)) return false; p += 8;
    if (!write_u64(p, argv1_va)) return false; p += 8;
    if (!write_u64(p, 0)) return false; p += 8;
    if (!write_u64(p, env0_va)) return false; p += 8;
    if (!write_u64(p, 0)) return false; p += 8;
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
        if (!write_u64(p, aux[i].type)) return false; p += 8;
        if (!write_u64(p, aux[i].value)) return false; p += 8;
    }
    image.stack_pointer = sp;
    return true;
}

static bool load_busybox(void) {
    if (twilight_busybox_elf_size < sizeof(struct elf64_ehdr)) return false;
    const struct elf64_ehdr *eh = (const struct elf64_ehdr *)twilight_busybox_elf;
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != ELFCLASS64 ||
        eh->ident[5] != ELFDATA2LSB || eh->ident[6] != EV_CURRENT ||
        eh->type != ET_EXEC || eh->machine != EM_X86_64 ||
        eh->version != EV_CURRENT || eh->ehsize != sizeof(*eh) ||
        eh->phentsize != sizeof(struct elf64_phdr) || eh->phnum == 0) return false;
    if (eh->phoff > twilight_busybox_elf_size ||
        (uint64_t)eh->phnum > (UINT64_MAX - eh->phoff) / sizeof(struct elf64_phdr) ||
        eh->phoff + (uint64_t)eh->phnum * sizeof(struct elf64_phdr) > twilight_busybox_elf_size)
        return false;

    image = (struct busybox_image){0};
    image.space = vmm_create_address_space();
    if (image.space == VMM_INVALID_SPACE) return false;
    image.entry = eh->entry;
    image.phnum = eh->phnum;
    image.phentsize = eh->phentsize;
    image.mmap_next = BUSYBOX_MMAP_BASE;

    const struct elf64_phdr *phdrs =
        (const struct elf64_phdr *)(twilight_busybox_elf + eh->phoff);
    bool entry_covered = false;
    uint64_t max_load_end = 0;

    for (uint16_t index = 0; index < eh->phnum; ++index) {
        const struct elf64_phdr *ph = &phdrs[index];
        if (ph->type == PT_PHDR) image.phdr = ph->vaddr;
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->filesz > ph->memsz || ph->offset > twilight_busybox_elf_size ||
            ph->filesz > twilight_busybox_elf_size - ph->offset ||
            ph->vaddr >= BUSYBOX_USER_TOP || ph->memsz > BUSYBOX_USER_TOP - ph->vaddr) {
            destroy_image();
            return false;
        }
        const uint64_t end = ph->vaddr + ph->memsz;
        if (end > max_load_end) max_load_end = end;
        if (eh->entry >= ph->vaddr && eh->entry < end && (ph->flags & PF_X) != 0)
            entry_covered = true;

        if (image.phdr == 0 && eh->phoff >= ph->offset &&
            eh->phoff + (uint64_t)eh->phnum * eh->phentsize <= ph->offset + ph->filesz)
            image.phdr = ph->vaddr + (eh->phoff - ph->offset);

        uint64_t page_end = 0;
        if (!align_up(end, &page_end)) { destroy_image(); return false; }
        uint64_t flags = VMM_FLAG_USER;
        if ((ph->flags & PF_W) != 0) flags |= VMM_FLAG_WRITE;
        if ((ph->flags & PF_X) == 0 && vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;
        for (uint64_t va = align_down(ph->vaddr); va < page_end; va += TWILIGHT_PAGE_SIZE) {
            if (add_page(va, flags) == 0) { destroy_image(); return false; }
        }
        if (ph->filesz != 0 &&
            !copy_to_user(ph->vaddr, twilight_busybox_elf + ph->offset, ph->filesz)) {
            destroy_image();
            return false;
        }
    }

    if (!entry_covered || image.phdr == 0 || !align_up(max_load_end, &image.brk_base)) {
        destroy_image();
        return false;
    }
    image.brk_current = image.brk_base;
    if (!build_initial_stack(eh)) { destroy_image(); return false; }

    for (size_t i = 0; i < image.page_count; ++i) {
        if (!protect_new_page(&image.pages[i])) { destroy_image(); return false; }
    }
    return true;
}

static bool map_runtime_page(uint64_t va, uint64_t flags) {
    if (find_page(va) != 0) return true;
    struct busybox_page *page = add_page(va, flags);
    return page != 0 && protect_new_page(page);
}

static int64_t sys_brk(uint64_t requested) {
    if (requested == 0) return (int64_t)image.brk_current;
    if (requested < image.brk_base || requested >= BUSYBOX_MMAP_BASE)
        return (int64_t)image.brk_current;
    uint64_t end = 0;
    if (!align_up(requested, &end)) return (int64_t)image.brk_current;
    uint64_t old_end = 0;
    if (!align_up(image.brk_current, &old_end)) return (int64_t)image.brk_current;
    uint64_t flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = old_end; va < end; va += TWILIGHT_PAGE_SIZE)
        if (!map_runtime_page(va, flags)) return (int64_t)image.brk_current;
    image.brk_current = requested;
    return (int64_t)requested;
}

static int64_t sys_mmap(uint64_t address, uint64_t length, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset) {
    (void)offset;
    if (length == 0) return -LINUX_EINVAL;
    if ((flags & MAP_PRIVATE) == 0 || (flags & MAP_ANONYMOUS) == 0 ||
        fd != UINT64_MAX) return -LINUX_ENOSYS;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;

    uint64_t map_length = 0;
    if (!align_up(length, &map_length)) return -LINUX_ENOMEM;
    uint64_t base;
    if ((flags & MAP_FIXED) != 0) {
        if ((address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) return -LINUX_EINVAL;
        base = address;
    } else {
        base = image.mmap_next;
        if (!align_up(base, &base)) return -LINUX_ENOMEM;
    }
    if (base >= BUSYBOX_MMAP_LIMIT || map_length > BUSYBOX_MMAP_LIMIT - base)
        return -LINUX_ENOMEM;

    uint64_t page_flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) page_flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE) {
        if (find_page(va) != 0 || !map_runtime_page(va, page_flags)) return -LINUX_ENOMEM;
    }
    if ((flags & MAP_FIXED) == 0) image.mmap_next = base + map_length + TWILIGHT_PAGE_SIZE;
    return (int64_t)base;
}

static int64_t sys_mprotect(uint64_t address, uint64_t length, uint64_t prot) {
    if ((address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || length == 0) return -LINUX_EINVAL;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;
    uint64_t end = 0;
    if (!align_up(address + length, &end)) return -LINUX_EINVAL;
    uint64_t flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = address; va < end; va += TWILIGHT_PAGE_SIZE) {
        struct busybox_page *page = find_page(va);
        if (page == 0) return -LINUX_ENOMEM;
        page->flags = flags;
        if (!vmm_protect_page(image.space, va, flags)) return -LINUX_EACCES;
    }
    return 0;
}

static int64_t sys_munmap(uint64_t address, uint64_t length) {
    if ((address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || length == 0) return -LINUX_EINVAL;
    uint64_t end = 0;
    if (!align_up(address + length, &end)) return -LINUX_EINVAL;
    for (uint64_t va = address; va < end; va += TWILIGHT_PAGE_SIZE) {
        for (size_t i = 0; i < image.page_count; ++i) {
            if (image.pages[i].va != va) continue;
            uint64_t old_phys = 0;
            if (!vmm_unmap_page(image.space, va, &old_phys)) return -LINUX_EINVAL;
            if (old_phys != 0) (void)pmm_free_page(old_phys);
            image.pages[i] = image.pages[image.page_count - 1u];
            --image.page_count;
            break;
        }
    }
    return 0;
}

static int64_t sys_write_common(int fd, uint64_t address, uint64_t length, bool prefix) {
    if (fd != 1 && fd != 2) return -LINUX_EBADF;
    if (!user_range(address, length, false)) return -LINUX_EFAULT;
    if (prefix) serial_write(fd == 1 ? "[busybox stdout] " : "[busybox stderr] ");
    const char *bytes = (const char *)(uintptr_t)address;
    for (uint64_t i = 0; i < length; ++i) serial_write_char(bytes[i]);
    write_seen = true;
    return (int64_t)length;
}

int64_t linux_busybox_syscall_dispatch(uint64_t number,
                                       uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                       uint64_t arg4, uint64_t arg5, uint64_t arg6) {
    if (!process_active) return -LINUX_ENOSYS;

    switch (number) {
    case SYS_WRITE:
        return sys_write_common((int)arg1, arg2, arg3, true);
    case SYS_WRITEV: {
        if ((int)arg1 != 1 && (int)arg1 != 2) return -LINUX_EBADF;
        if (arg3 > 64 || !user_range(arg2, arg3 * sizeof(struct linux_iovec), false))
            return -LINUX_EFAULT;
        const struct linux_iovec *iov = (const struct linux_iovec *)(uintptr_t)arg2;
        int64_t total = 0;
        serial_write((int)arg1 == 1 ? "[busybox stdout] " : "[busybox stderr] ");
        for (uint64_t i = 0; i < arg3; ++i) {
            const int64_t rc = sys_write_common((int)arg1, iov[i].base, iov[i].len, false);
            if (rc < 0) return rc;
            total += rc;
        }
        return total;
    }
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        exit_status = (int)(arg1 & 0xffu);
        exit_seen = true;
        write_msr(IA32_FS_BASE_MSR, 0);
        fs_base = 0;
        return LINUX_EXIT_SENTINEL;
    case SYS_ARCH_PRCTL:
        if (arg1 == ARCH_SET_FS) {
            if (arg2 >= BUSYBOX_USER_TOP) return -LINUX_EPERM;
            fs_base = arg2;
            write_msr(IA32_FS_BASE_MSR, arg2);
            return 0;
        }
        if (arg1 == ARCH_GET_FS) return user_store_u64(arg2, fs_base) ? 0 : -LINUX_EFAULT;
        if (arg1 == ARCH_SET_GS) return arg2 == 0 ? 0 : -LINUX_EPERM;
        if (arg1 == ARCH_GET_GS) return user_store_u64(arg2, 0) ? 0 : -LINUX_EFAULT;
        return -LINUX_EINVAL;
    case SYS_SET_TID_ADDRESS:
        return 1;
    case SYS_SET_ROBUST_LIST:
        return 0;
    case SYS_GETPID:
    case SYS_GETTID:
        return 1;
    case SYS_GETPPID:
        return 0;
    case SYS_GETUID:
    case SYS_GETGID:
    case SYS_GETEUID:
    case SYS_GETEGID:
        return 0;
    case SYS_BRK:
        return sys_brk(arg1);
    case SYS_MMAP:
        return sys_mmap(arg1, arg2, arg3, arg4, arg5, arg6);
    case SYS_MPROTECT:
        return sys_mprotect(arg1, arg2, arg3);
    case SYS_MUNMAP:
        return sys_munmap(arg1, arg2);
    case SYS_MADVISE:
        return 0;
    case SYS_RT_SIGACTION:
        if (arg3 != 0 && !zero_user(arg3, 32)) return -LINUX_EFAULT;
        return 0;
    case SYS_RT_SIGPROCMASK:
        if (arg4 > 128) return -LINUX_EINVAL;
        if (arg3 != 0 && !zero_user(arg3, arg4)) return -LINUX_EFAULT;
        return 0;
    case SYS_PRLIMIT64:
        if (arg4 != 0) {
            if (!user_store_u64(arg4, UINT64_MAX) || !user_store_u64(arg4 + 8, UINT64_MAX))
                return -LINUX_EFAULT;
        }
        return 0;
    case SYS_GETRANDOM:
        if (!user_range(arg1, arg2, true)) return -LINUX_EFAULT;
        for (uint64_t i = 0; i < arg2; ++i) {
            random_state = random_state * 6364136223846793005ull + 1ull;
            *(volatile uint8_t *)(uintptr_t)(arg1 + i) = (uint8_t)(random_state >> 32);
        }
        return (int64_t)arg2;
    case SYS_CLOCK_GETTIME:
        if (!zero_user(arg2, 16)) return -LINUX_EFAULT;
        return 0;
    case SYS_FSTAT:
        if ((int)arg1 < 0 || (int)arg1 > 2) return -LINUX_EBADF;
        if (!zero_user(arg2, 144)) return -LINUX_EFAULT;
        if (!user_store_u64(arg2 + 16, 1) ||
            !user_store_u32(arg2 + 24, 0020000u | 0666u)) return -LINUX_EFAULT;
        return 0;
    case SYS_IOCTL:
        return -LINUX_ENOTTY;
    case SYS_FCNTL:
        if ((int)arg1 < 0 || (int)arg1 > 2) return -LINUX_EBADF;
        if (arg2 == 1) return 0; /* F_GETFD */
        if (arg2 == 3) return (int)arg1 == 0 ? 0 : 1; /* F_GETFL */
        return 0;
    case SYS_READ:
        if ((int)arg1 != 0) return -LINUX_EBADF;
        if (!user_range(arg2, arg3, true)) return -LINUX_EFAULT;
        return 0; /* EOF */
    case SYS_CLOSE:
        return ((int)arg1 >= 0 && (int)arg1 <= 2) ? 0 : -LINUX_EBADF;
    case SYS_LSEEK:
        return -LINUX_ESPIPE;
    case SYS_UNAME: {
        if (!zero_user(arg1, 390)) return -LINUX_EFAULT;
        const char sysname[] = "Linux";
        const char nodename[] = "nox";
        const char release[] = "6.0.0-twilight";
        const char version[] = "Twilight Linux ABI";
        const char machine[] = "x86_64";
        if (!copy_to_user(arg1 + 0, sysname, sizeof(sysname)) ||
            !copy_to_user(arg1 + 65, nodename, sizeof(nodename)) ||
            !copy_to_user(arg1 + 130, release, sizeof(release)) ||
            !copy_to_user(arg1 + 195, version, sizeof(version)) ||
            !copy_to_user(arg1 + 260, machine, sizeof(machine))) return -LINUX_EFAULT;
        return 0;
    }
    case SYS_FUTEX:
        return 0;
    case SYS_RSEQ:
        return -LINUX_ENOSYS;
    case SYS_ACCESS:
    case SYS_OPENAT:
    case SYS_NEWFSTATAT:
    case SYS_READLINK:
    case SYS_GETCWD:
        return -LINUX_ENOENT;
    default:
        log_unknown_syscall(number);
        return -LINUX_ENOSYS;
    }
}

static int busybox_test_init(void) {
    trace("official BusyBox 1.35.0 x86_64-musl test start");
    if (!gdt_is_initialized() || vmm_kernel_space() == VMM_INVALID_SPACE) {
        trace("GDT/VMM unavailable");
        return 0;
    }
    if (!syscall_init()) {
        trace("SYSCALL/SYSRET setup failed");
        return 0;
    }

    struct pmm_stats before, after;
    pmm_get_stats(&before);
    if (!load_busybox()) {
        trace("BusyBox ELF loader rejected the official binary");
        return 0;
    }
    trace("official BusyBox ELF mapped; Linux argc/argv/envp/auxv stack ready");

    uint64_t translated = 0, flags = 0;
    if (!vmm_translate(image.space, (uint64_t)(uintptr_t)&linux_syscall_entry,
                       &translated, &flags)) {
        trace("LSTAR entry missing from BusyBox process CR3");
        destroy_image();
        return 0;
    }

    const vmm_space_t kernel_space = vmm_kernel_space();
    uint64_t rflags = 0;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    const bool interrupts_were_enabled = (rflags & (1ull << 9)) != 0;

    process_active = true;
    write_seen = false;
    exit_seen = false;
    unknown_syscall_seen = false;
    exit_status = -1;
    trace("entering UNMODIFIED BusyBox as /bin/echo at CPL3");
    user_mode_enter(image.entry, image.stack_pointer, image.space, kernel_space);
    process_active = false;

    if (interrupts_were_enabled) __asm__ volatile ("sti" ::: "memory");
    else __asm__ volatile ("cli" ::: "memory");

    const bool success = vmm_current_space() == kernel_space &&
                         write_seen && exit_seen && exit_status == 0;
    if (success)
        trace(unknown_syscall_seen ?
              "PASS: unmodified BusyBox echo ran successfully (optional unsupported syscall(s) were tolerated)" :
              "PASS: unmodified BusyBox echo ran through Twilight's Linux ABI");
    else
        trace("FAIL: BusyBox returned without a successful echo + exit_group sequence");

    destroy_image();
    pmm_get_stats(&after);
    if (after.free_pages != before.free_pages)
        trace("warning: BusyBox test changed PMM free-page count");
    return 0;
}

module_init(busybox_test_init);

#endif
