#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/module.h>
#include <twilight/gdt.h>
#include <twilight/interrupts.h>
#include <twilight/pmm.h>
#include <twilight/serial.h>
#include <twilight/tty.h>
#include <twilight/vmm.h>

#if TWILIGHT_BUSYBOX_SELF_TEST

#define IA32_EFER_MSR    0xc0000080u
#define IA32_STAR_MSR    0xc0000081u
#define IA32_LSTAR_MSR   0xc0000082u
#define IA32_FMASK_MSR   0xc0000084u
#define IA32_FS_BASE_MSR 0xc0000100u
#define IA32_EFER_SCE    (1ull << 0)

#define LINUX_EXIT_SENTINEL ((int64_t)INT64_MIN)

#define SYS_READ             0ull
#define SYS_WRITE            1ull
#define SYS_OPEN             2ull
#define SYS_CLOSE            3ull
#define SYS_STAT             4ull
#define SYS_FSTAT            5ull
#define SYS_LSTAT            6ull
#define SYS_POLL             7ull
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
#define SYS_SELECT          23ull
#define SYS_MADVISE         28ull
#define SYS_DUP             32ull
#define SYS_DUP2            33ull
#define SYS_NANOSLEEP       35ull
#define SYS_GETPID          39ull
#define SYS_EXIT            60ull
#define SYS_WAIT4           61ull
#define SYS_KILL            62ull
#define SYS_UNAME           63ull
#define SYS_FCNTL           72ull
#define SYS_GETCWD          79ull
#define SYS_CHDIR           80ull
#define SYS_UMASK           95ull
#define SYS_GETTIMEOFDAY    96ull
#define SYS_GETRLIMIT       97ull
#define SYS_GETUID         102ull
#define SYS_GETGID         104ull
#define SYS_GETEUID        107ull
#define SYS_GETEGID        108ull
#define SYS_SETPGID        109ull
#define SYS_GETPPID        110ull
#define SYS_GETPGRP        111ull
#define SYS_SETSID         112ull
#define SYS_GETGROUPS      115ull
#define SYS_GETRESUID      118ull
#define SYS_GETRESGID      120ull
#define SYS_GETPGID        121ull
#define SYS_GETSID         124ull
#define SYS_SIGALTSTACK    131ull
#define SYS_PRCTL          157ull
#define SYS_ARCH_PRCTL     158ull
#define SYS_GETTID         186ull
#define SYS_FUTEX          202ull
#define SYS_GETDENTS64     217ull
#define SYS_SET_TID_ADDRESS 218ull
#define SYS_CLOCK_GETTIME  228ull
#define SYS_EXIT_GROUP     231ull
#define SYS_TGKILL         234ull
#define SYS_OPENAT         257ull
#define SYS_NEWFSTATAT     262ull
#define SYS_PSELECT6       270ull
#define SYS_PPOLL          271ull
#define SYS_SET_ROBUST_LIST 273ull
#define SYS_DUP3           292ull
#define SYS_PIPE2          293ull
#define SYS_PRLIMIT64      302ull
#define SYS_GETRANDOM      318ull
#define SYS_RSEQ           334ull

#define LINUX_EPERM       1
#define LINUX_ENOENT      2
#define LINUX_EINTR       4
#define LINUX_EIO         5
#define LINUX_EBADF       9
#define LINUX_ECHILD     10
#define LINUX_EAGAIN     11
#define LINUX_ENOMEM     12
#define LINUX_EACCES     13
#define LINUX_EFAULT     14
#define LINUX_EBUSY      16
#define LINUX_EEXIST     17
#define LINUX_ENOTDIR    20
#define LINUX_EISDIR     21
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

#define AT_NULL     0ull
#define AT_PHDR     3ull
#define AT_PHENT    4ull
#define AT_PHNUM    5ull
#define AT_PAGESZ   6ull
#define AT_BASE     7ull
#define AT_FLAGS    8ull
#define AT_ENTRY    9ull
#define AT_UID     11ull
#define AT_EUID    12ull
#define AT_GID     13ull
#define AT_EGID    14ull
#define AT_PLATFORM 15ull
#define AT_HWCAP   16ull
#define AT_CLKTCK  17ull
#define AT_SECURE  23ull
#define AT_RANDOM  25ull
#define AT_HWCAP2  26ull
#define AT_EXECFN  31ull

#define SHELL_USER_TOP      0x0000800000000000ull
#define SHELL_STACK_TOP     0x00007fffffffe000ull
#define SHELL_STACK_PAGES   32u
#define SHELL_MAX_PAGES     768u
#define SHELL_MMAP_BASE     0x0000003000000000ull
#define SHELL_MMAP_LIMIT    0x0000004000000000ull

#define TCGETS      0x5401ull
#define TCSETS      0x5402ull
#define TCSETSW     0x5403ull
#define TCSETSF     0x5404ull
#define TCFLSH      0x540bull
#define TIOCSCTTY   0x540eull
#define TIOCGPGRP   0x540full
#define TIOCSPGRP   0x5410ull
#define TIOCGWINSZ  0x5413ull
#define FIONREAD    0x541bull
#define TIOCGSID    0x5429ull

#define POLLIN  0x0001
#define POLLOUT 0x0004

#define S_IFCHR  0020000u
#define S_IFDIR  0040000u
#define S_IFREG  0100000u

extern const uint8_t twilight_busybox_elf[];
extern const size_t twilight_busybox_elf_size;
extern void linux_syscall_entry(void);
extern void user_mode_enter_interruptible(uint64_t instruction_pointer,
                                          uint64_t stack_pointer,
                                          vmm_space_t user_space,
                                          vmm_space_t kernel_space);
extern int64_t linux_busybox_syscall_dispatch(uint64_t number,
                                              uint64_t arg1, uint64_t arg2,
                                              uint64_t arg3, uint64_t arg4,
                                              uint64_t arg5, uint64_t arg6);

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

struct shell_page {
    uint64_t va;
    uint64_t phys;
    uint64_t flags;
};

struct shell_image {
    vmm_space_t space;
    uint64_t entry;
    uint64_t stack_pointer;
    uint64_t phdr;
    uint16_t phnum;
    uint16_t phentsize;
    uint64_t brk_base;
    uint64_t brk_current;
    uint64_t mmap_next;
    struct shell_page pages[SHELL_MAX_PAGES];
    size_t page_count;
};

struct linux_iovec {
    uint64_t base;
    uint64_t len;
};

struct __attribute__((packed)) linux_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct __attribute__((packed)) linux_termios {
    uint32_t iflag;
    uint32_t oflag;
    uint32_t cflag;
    uint32_t lflag;
    uint8_t line;
    uint8_t cc[19];
};

struct __attribute__((packed)) linux_winsize {
    uint16_t rows;
    uint16_t cols;
    uint16_t xpixel;
    uint16_t ypixel;
};

struct aux_pair {
    uint64_t type;
    uint64_t value;
};

static struct shell_image image;
static bool shell_active;
static bool shell_exit_seen;
static int shell_exit_status;
static uint64_t fs_base;
static uint64_t random_state = 0x5348454c4c4e4f58ull;
static char current_directory[64] = "/";
static struct linux_termios terminal_settings;
static int foreground_pgrp = 1;

static void trace(const char *text) {
    serial_write("[serial] busybox-shell: ");
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
    size_t n = 0;
    if (text == 0) return 0;
    while (text[n] != '\0') ++n;
    return n;
}

static bool string_equal(const char *a, const char *b) {
    if (a == 0 || b == 0) return false;
    size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == b[i];
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

static void log_unknown(uint64_t number) {
    serial_write("[linux:ash] unsupported syscall ");
    serial_u64(number);
    serial_write(" -> -ENOSYS\n");
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

static struct shell_page *find_page(uint64_t address) {
    const uint64_t page_va = align_down(address);
    for (size_t i = 0; i < image.page_count; ++i)
        if (image.pages[i].va == page_va) return &image.pages[i];
    return 0;
}

static struct shell_page *add_page(uint64_t va, uint64_t final_flags) {
    if (image.space == VMM_INVALID_SPACE ||
        (va & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || va >= SHELL_USER_TOP)
        return 0;

    struct shell_page *existing = find_page(va);
    if (existing != 0) {
        existing->flags |= final_flags & VMM_FLAG_WRITE;
        if ((final_flags & VMM_FLAG_NO_EXECUTE) == 0)
            existing->flags &= ~VMM_FLAG_NO_EXECUTE;
        return existing;
    }

    if (image.page_count >= SHELL_MAX_PAGES) return 0;
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

    struct shell_page *page = &image.pages[image.page_count++];
    page->va = va;
    page->phys = phys;
    page->flags = final_flags | VMM_FLAG_USER;
    return page;
}

static bool user_range(uint64_t address, uint64_t length, bool writable) {
    if (!shell_active || image.space == VMM_INVALID_SPACE) return false;
    if (length == 0) return true;
    if (address >= SHELL_USER_TOP || address > UINT64_MAX - length ||
        address + length > SHELL_USER_TOP) return false;
    const uint64_t end = address + length;
    for (uint64_t page = align_down(address); page < end; page += TWILIGHT_PAGE_SIZE) {
        uint64_t phys = 0, flags = 0;
        if (!vmm_translate(image.space, page, &phys, &flags) ||
            (flags & VMM_FLAG_USER) == 0 ||
            (writable && (flags & VMM_FLAG_WRITE) == 0)) return false;
    }
    return true;
}

static bool copy_to_process(uint64_t address, const void *source, uint64_t length) {
    const uint8_t *input = source;
    while (length != 0) {
        struct shell_page *page = find_page(address);
        if (page == 0) return false;
        uint8_t *direct = pmm_phys_to_virt(page->phys);
        if (direct == 0) return false;
        const uint64_t offset = address & (TWILIGHT_PAGE_SIZE - 1ull);
        uint64_t chunk = TWILIGHT_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        bytes_copy(direct + offset, input, (size_t)chunk);
        input += chunk;
        address += chunk;
        length -= chunk;
    }
    return true;
}

static bool copy_from_process(void *destination, uint64_t address, uint64_t length) {
    uint8_t *output = destination;
    while (length != 0) {
        struct shell_page *page = find_page(address);
        if (page == 0) return false;
        const uint8_t *direct = pmm_phys_to_virt(page->phys);
        if (direct == 0) return false;
        const uint64_t offset = address & (TWILIGHT_PAGE_SIZE - 1ull);
        uint64_t chunk = TWILIGHT_PAGE_SIZE - offset;
        if (chunk > length) chunk = length;
        bytes_copy(output, direct + offset, (size_t)chunk);
        output += chunk;
        address += chunk;
        length -= chunk;
    }
    return true;
}

static bool user_copy_out(uint64_t address, const void *source, uint64_t length) {
    return user_range(address, length, true) && copy_to_process(address, source, length);
}

static bool user_copy_in(void *destination, uint64_t address, uint64_t length) {
    return user_range(address, length, false) && copy_from_process(destination, address, length);
}

static bool user_zero(uint64_t address, uint64_t length) {
    static const uint8_t zeros[64] = {0};
    if (!user_range(address, length, true)) return false;
    while (length != 0) {
        uint64_t chunk = length > sizeof(zeros) ? sizeof(zeros) : length;
        if (!copy_to_process(address, zeros, chunk)) return false;
        address += chunk;
        length -= chunk;
    }
    return true;
}

static bool user_store_u32(uint64_t address, uint32_t value) {
    return user_copy_out(address, &value, sizeof(value));
}

static bool user_store_u64(uint64_t address, uint64_t value) {
    return user_copy_out(address, &value, sizeof(value));
}

static bool copy_user_string(uint64_t address, char *out, size_t capacity) {
    if (out == 0 || capacity == 0) return false;
    for (size_t i = 0; i < capacity; ++i) {
        char c = 0;
        if (!user_copy_in(&c, address + i, 1)) return false;
        out[i] = c;
        if (c == '\0') return true;
    }
    out[capacity - 1u] = '\0';
    return false;
}

static bool protect_page(struct shell_page *page) {
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
    image = (struct shell_image){0};
}

static bool push_stack_bytes(uint64_t *cursor, const void *data, size_t length,
                             uint64_t *user_address) {
    if (cursor == 0 || data == 0 || length == 0 || *cursor < length) return false;
    *cursor -= length;
    if (!copy_to_process(*cursor, data, length)) return false;
    if (user_address != 0) *user_address = *cursor;
    return true;
}

static bool push_stack_string(uint64_t *cursor, const char *text, uint64_t *user_address) {
    return push_stack_bytes(cursor, text, string_length(text) + 1u, user_address);
}

static bool stack_u64(uint64_t address, uint64_t value) {
    return copy_to_process(address, &value, sizeof(value));
}

static bool build_initial_stack(const struct elf64_ehdr *eh) {
    const uint64_t stack_base = SHELL_STACK_TOP -
                                (uint64_t)SHELL_STACK_PAGES * TWILIGHT_PAGE_SIZE;
    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = stack_base; va < SHELL_STACK_TOP; va += TWILIGHT_PAGE_SIZE)
        if (add_page(va, stack_flags) == 0) return false;

    uint64_t cursor = SHELL_STACK_TOP;
    const char argv0[] = "/bin/sh";
    const char argv1[] = "-i";
    const char env0[] = "PATH=/bin:/usr/bin";
    const char env1[] = "HOME=/";
    const char env2[] = "TERM=xterm";
    const char env3[] = "PS1=nox# ";
    const char platform[] = "x86_64";
    const uint8_t random_bytes[16] = {
        0x4e,0x6f,0x78,0x54,0x77,0x69,0x6c,0x69,
        0x67,0x68,0x74,0x41,0x73,0x68,0x21,0x21,
    };

    uint64_t argv0_va=0, argv1_va=0, env0_va=0, env1_va=0, env2_va=0, env3_va=0;
    uint64_t platform_va=0, random_va=0;
    if (!push_stack_bytes(&cursor, random_bytes, sizeof(random_bytes), &random_va) ||
        !push_stack_string(&cursor, platform, &platform_va) ||
        !push_stack_string(&cursor, env3, &env3_va) ||
        !push_stack_string(&cursor, env2, &env2_va) ||
        !push_stack_string(&cursor, env1, &env1_va) ||
        !push_stack_string(&cursor, env0, &env0_va) ||
        !push_stack_string(&cursor, argv1, &argv1_va) ||
        !push_stack_string(&cursor, argv0, &argv0_va)) return false;

    const struct aux_pair aux[] = {
        { AT_PHDR, image.phdr }, { AT_PHENT, image.phentsize }, { AT_PHNUM, image.phnum },
        { AT_PAGESZ, TWILIGHT_PAGE_SIZE }, { AT_BASE, 0 }, { AT_FLAGS, 0 },
        { AT_ENTRY, eh->entry }, { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 },
        { AT_EGID, 0 }, { AT_PLATFORM, platform_va }, { AT_HWCAP, 0 },
        { AT_CLKTCK, 100 }, { AT_SECURE, 0 }, { AT_RANDOM, random_va },
        { AT_HWCAP2, 0 }, { AT_EXECFN, argv0_va }, { AT_NULL, 0 },
    };

    const size_t aux_words = sizeof(aux) / sizeof(aux[0]) * 2u;
    const size_t table_words = 1u + 3u + 5u + aux_words;
    const uint64_t table_bytes = (uint64_t)table_words * 8ull;
    if (cursor < stack_base + table_bytes) return false;
    uint64_t p = (cursor - table_bytes) & ~15ull;
    image.stack_pointer = p;

    if (!stack_u64(p, 2)) return false; p += 8;
    if (!stack_u64(p, argv0_va)) return false; p += 8;
    if (!stack_u64(p, argv1_va)) return false; p += 8;
    if (!stack_u64(p, 0)) return false; p += 8;
    if (!stack_u64(p, env0_va)) return false; p += 8;
    if (!stack_u64(p, env1_va)) return false; p += 8;
    if (!stack_u64(p, env2_va)) return false; p += 8;
    if (!stack_u64(p, env3_va)) return false; p += 8;
    if (!stack_u64(p, 0)) return false; p += 8;
    for (size_t i = 0; i < sizeof(aux)/sizeof(aux[0]); ++i) {
        if (!stack_u64(p, aux[i].type)) return false; p += 8;
        if (!stack_u64(p, aux[i].value)) return false; p += 8;
    }
    return true;
}

static bool load_shell(void) {
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

    image = (struct shell_image){0};
    image.space = vmm_create_address_space();
    if (image.space == VMM_INVALID_SPACE) return false;
    image.entry = eh->entry;
    image.phnum = eh->phnum;
    image.phentsize = eh->phentsize;
    image.mmap_next = SHELL_MMAP_BASE;

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
            ph->vaddr >= SHELL_USER_TOP || ph->memsz > SHELL_USER_TOP - ph->vaddr) {
            destroy_image(); return false;
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
        for (uint64_t va = align_down(ph->vaddr); va < page_end; va += TWILIGHT_PAGE_SIZE)
            if (add_page(va, flags) == 0) { destroy_image(); return false; }
        if (ph->filesz != 0 &&
            !copy_to_process(ph->vaddr, twilight_busybox_elf + ph->offset, ph->filesz)) {
            destroy_image(); return false;
        }
    }

    if (!entry_covered || image.phdr == 0 || !align_up(max_load_end, &image.brk_base)) {
        destroy_image(); return false;
    }
    image.brk_current = image.brk_base;
    if (!build_initial_stack(eh)) { destroy_image(); return false; }
    for (size_t i = 0; i < image.page_count; ++i)
        if (!protect_page(&image.pages[i])) { destroy_image(); return false; }
    return true;
}

static bool map_runtime_page(uint64_t va, uint64_t flags) {
    if (find_page(va) != 0) return true;
    struct shell_page *page = add_page(va, flags);
    return page != 0 && protect_page(page);
}

static int64_t sys_brk(uint64_t requested) {
    if (requested == 0) return (int64_t)image.brk_current;
    if (requested < image.brk_base || requested >= SHELL_MMAP_BASE)
        return (int64_t)image.brk_current;
    uint64_t end = 0, old_end = 0;
    if (!align_up(requested, &end) || !align_up(image.brk_current, &old_end))
        return (int64_t)image.brk_current;
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
    if ((flags & MAP_PRIVATE) == 0 || (flags & MAP_ANONYMOUS) == 0 || fd != UINT64_MAX)
        return -LINUX_ENOSYS;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;
    uint64_t map_length = 0;
    if (!align_up(length, &map_length)) return -LINUX_ENOMEM;
    uint64_t base = address;
    if ((flags & MAP_FIXED) == 0) {
        base = image.mmap_next;
        if (!align_up(base, &base)) return -LINUX_ENOMEM;
    } else if ((base & (TWILIGHT_PAGE_SIZE - 1ull)) != 0) {
        return -LINUX_EINVAL;
    }
    if (base >= SHELL_MMAP_LIMIT || map_length > SHELL_MMAP_LIMIT - base)
        return -LINUX_ENOMEM;
    uint64_t page_flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) page_flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = base; va < base + map_length; va += TWILIGHT_PAGE_SIZE)
        if (find_page(va) != 0 || !map_runtime_page(va, page_flags)) return -LINUX_ENOMEM;
    if ((flags & MAP_FIXED) == 0) image.mmap_next = base + map_length + TWILIGHT_PAGE_SIZE;
    return (int64_t)base;
}

static int64_t sys_mprotect(uint64_t address, uint64_t length, uint64_t prot) {
    if ((address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || length == 0) return -LINUX_EINVAL;
    if ((prot & PROT_WRITE) != 0 && (prot & PROT_EXEC) != 0) return -LINUX_EACCES;
    uint64_t end = 0;
    if (address > UINT64_MAX - length || !align_up(address + length, &end)) return -LINUX_EINVAL;
    uint64_t flags = VMM_FLAG_USER;
    if ((prot & PROT_WRITE) != 0) flags |= VMM_FLAG_WRITE;
    if ((prot & PROT_EXEC) == 0 && vmm_nx_supported()) flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = address; va < end; va += TWILIGHT_PAGE_SIZE) {
        struct shell_page *page = find_page(va);
        if (page == 0) return -LINUX_ENOMEM;
        page->flags = flags;
        if (!vmm_protect_page(image.space, va, flags)) return -LINUX_EACCES;
    }
    return 0;
}

static int64_t sys_munmap(uint64_t address, uint64_t length) {
    if ((address & (TWILIGHT_PAGE_SIZE - 1ull)) != 0 || length == 0) return -LINUX_EINVAL;
    uint64_t end = 0;
    if (address > UINT64_MAX - length || !align_up(address + length, &end)) return -LINUX_EINVAL;
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

static bool fd_is_tty(int fd) {
    return fd >= 0 && fd <= 9;
}

static int64_t sys_write_tty(int fd, uint64_t address, uint64_t length) {
    if (!fd_is_tty(fd)) return -LINUX_EBADF;
    if (!user_range(address, length, false)) return -LINUX_EFAULT;
    char buffer[128];
    uint64_t done = 0;
    while (done < length) {
        uint64_t chunk = length - done;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        if (!user_copy_in(buffer, address + done, chunk)) return -LINUX_EFAULT;
        tty_write(buffer, (size_t)chunk);
        done += chunk;
    }
    return (int64_t)length;
}

static int64_t sys_read_tty(int fd, uint64_t address, uint64_t length) {
    if (fd != 0 && fd != 3) return -LINUX_EBADF;
    if (length == 0) return 0;
    if (!user_range(address, length, true)) return -LINUX_EFAULT;

    char c = 0;
    if (tty_read_char_blocking(&c) != 1) return -LINUX_EIO;
    if (!user_copy_out(address, &c, 1)) return -LINUX_EFAULT;

    /* Returning one byte is legal for a terminal and works naturally with
     * BusyBox's interactive line editor. */
    return 1;
}

static void init_termios(void) {
    bytes_zero(&terminal_settings, sizeof(terminal_settings));
    terminal_settings.iflag = 0x00000502u; /* BRKINT | ICRNL | IXON */
    terminal_settings.oflag = 0x00000005u; /* OPOST | ONLCR */
    terminal_settings.cflag = 0x000000bfu; /* B38400 | CS8 | CREAD */
    terminal_settings.lflag = 0x00008a3bu; /* ISIG|ICANON|ECHO...|IEXTEN */
    terminal_settings.cc[0] = 3;    /* VINTR ^C */
    terminal_settings.cc[1] = 28;   /* VQUIT ^\\ */
    terminal_settings.cc[2] = 127;  /* VERASE */
    terminal_settings.cc[3] = 21;   /* VKILL ^U */
    terminal_settings.cc[4] = 4;    /* VEOF ^D */
    terminal_settings.cc[5] = 0;    /* VTIME */
    terminal_settings.cc[6] = 1;    /* VMIN */
    terminal_settings.cc[8] = 17;   /* VSTART ^Q */
    terminal_settings.cc[9] = 19;   /* VSTOP ^S */
    terminal_settings.cc[10] = 26;  /* VSUSP ^Z */
}

static int64_t sys_ioctl(uint64_t fd_value, uint64_t request, uint64_t argument) {
    const int fd = (int)fd_value;
    if (!fd_is_tty(fd)) return -LINUX_EBADF;

    switch (request) {
    case TCGETS:
        return user_copy_out(argument, &terminal_settings, sizeof(terminal_settings)) ? 0 : -LINUX_EFAULT;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return user_copy_in(&terminal_settings, argument, sizeof(terminal_settings)) ? 0 : -LINUX_EFAULT;
    case TIOCGWINSZ: {
        const struct linux_winsize size = { .rows = 25, .cols = 80, .xpixel = 0, .ypixel = 0 };
        return user_copy_out(argument, &size, sizeof(size)) ? 0 : -LINUX_EFAULT;
    }
    case TIOCGPGRP:
    case TIOCGSID: {
        int value = foreground_pgrp;
        return user_copy_out(argument, &value, sizeof(value)) ? 0 : -LINUX_EFAULT;
    }
    case TIOCSPGRP: {
        int value = 0;
        if (!user_copy_in(&value, argument, sizeof(value))) return -LINUX_EFAULT;
        if (value > 0) foreground_pgrp = value;
        return 0;
    }
    case FIONREAD: {
        int available = tty_input_available() ? 1 : 0;
        return user_copy_out(argument, &available, sizeof(available)) ? 0 : -LINUX_EFAULT;
    }
    case TCFLSH:
    case TIOCSCTTY:
        return 0;
    default:
        return -LINUX_ENOTTY;
    }
}

static int64_t fill_stat(uint64_t address, uint32_t mode) {
    if (!user_zero(address, 144)) return -LINUX_EFAULT;
    uint64_t one = 1;
    if (!user_copy_out(address + 16, &one, sizeof(one)) ||
        !user_store_u32(address + 24, mode)) return -LINUX_EFAULT;
    return 0;
}

static bool path_is_known(const char *path) {
    return string_equal(path, "/") || string_equal(path, ".") ||
           string_equal(path, "/dev/tty") || string_equal(path, "/dev/null") ||
           string_equal(path, "/bin/sh") || string_equal(path, "/bin/busybox");
}

static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {
    char path[128];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (!path_is_known(path)) return -LINUX_ENOENT;
    if (string_equal(path, "/") || string_equal(path, "."))
        return fill_stat(stat_address, S_IFDIR | 0755u);
    if (string_equal(path, "/dev/tty") || string_equal(path, "/dev/null"))
        return fill_stat(stat_address, S_IFCHR | 0666u);
    return fill_stat(stat_address, S_IFREG | 0755u);
}

static int64_t sys_open_path(uint64_t path_address) {
    char path[128];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (string_equal(path, "/dev/tty")) return 3;
    if (string_equal(path, "/dev/null")) return 4;
    return -LINUX_ENOENT;
}

static int64_t sys_poll(uint64_t fds_address, uint64_t count, int64_t timeout) {
    if (count > 32) return -LINUX_EINVAL;
    if (count != 0 && !user_range(fds_address, count * sizeof(struct linux_pollfd), true))
        return -LINUX_EFAULT;

    for (;;) {
        int ready = 0;
        bool wants_input = false;
        for (uint64_t i = 0; i < count; ++i) {
            struct linux_pollfd pfd;
            if (!user_copy_in(&pfd, fds_address + i * sizeof(pfd), sizeof(pfd)))
                return -LINUX_EFAULT;
            pfd.revents = 0;
            if (pfd.fd == 0 || pfd.fd == 3) {
                if ((pfd.events & POLLIN) != 0) {
                    wants_input = true;
                    if (tty_input_available()) pfd.revents |= POLLIN;
                }
            } else if (fd_is_tty(pfd.fd) && (pfd.events & POLLOUT) != 0) {
                pfd.revents |= POLLOUT;
            }
            if (pfd.revents != 0) ++ready;
            if (!user_copy_out(fds_address + i * sizeof(pfd), &pfd, sizeof(pfd)))
                return -LINUX_EFAULT;
        }
        if (ready != 0 || timeout == 0 || !wants_input) return ready;
        tty_wait_for_input();
        if (timeout > 0) timeout = 0;
    }
}

static int64_t sys_fcntl(int fd, uint64_t command, uint64_t argument) {
    (void)argument;
    if (!fd_is_tty(fd)) return -LINUX_EBADF;
    switch (command) {
    case 0: return 3;                  /* F_DUPFD */
    case 1: return 0;                  /* F_GETFD */
    case 2: return 0;                  /* F_SETFD */
    case 3: return fd == 0 ? 0 : 1;    /* F_GETFL */
    case 4: return 0;                  /* F_SETFL */
    case 1030: return 3;               /* F_DUPFD_CLOEXEC */
    default: return 0;
    }
}

static int64_t sys_uname(uint64_t address) {
    if (!user_zero(address, 390)) return -LINUX_EFAULT;
    const char sysname[] = "Linux";
    const char nodename[] = "nox";
    const char release[] = "6.0.0-twilight";
    const char version[] = "Twilight Linux ABI";
    const char machine[] = "x86_64";
    if (!user_copy_out(address + 0, sysname, sizeof(sysname)) ||
        !user_copy_out(address + 65, nodename, sizeof(nodename)) ||
        !user_copy_out(address + 130, release, sizeof(release)) ||
        !user_copy_out(address + 195, version, sizeof(version)) ||
        !user_copy_out(address + 260, machine, sizeof(machine))) return -LINUX_EFAULT;
    return 0;
}

static int64_t shell_dispatch(uint64_t number,
                              uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6) {
    switch (number) {
    case SYS_READ:
        if ((int)a1 == 4) return 0; /* /dev/null */
        return sys_read_tty((int)a1, a2, a3);
    case SYS_WRITE:
        if ((int)a1 == 4) return (int64_t)a3;
        return sys_write_tty((int)a1, a2, a3);
    case SYS_WRITEV: {
        if (!fd_is_tty((int)a1)) return -LINUX_EBADF;
        if (a3 > 64 || !user_range(a2, a3 * sizeof(struct linux_iovec), false))
            return -LINUX_EFAULT;
        int64_t total = 0;
        for (uint64_t i = 0; i < a3; ++i) {
            struct linux_iovec iov;
            if (!user_copy_in(&iov, a2 + i * sizeof(iov), sizeof(iov))) return -LINUX_EFAULT;
            const int64_t rc = sys_write_tty((int)a1, iov.base, iov.len);
            if (rc < 0) return rc;
            total += rc;
        }
        return total;
    }
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        shell_exit_status = (int)(a1 & 0xffu);
        shell_exit_seen = true;
        write_msr(IA32_FS_BASE_MSR, 0);
        fs_base = 0;
        return LINUX_EXIT_SENTINEL;
    case SYS_ARCH_PRCTL:
        if (a1 == ARCH_SET_FS) {
            if (a2 >= SHELL_USER_TOP) return -LINUX_EPERM;
            fs_base = a2;
            write_msr(IA32_FS_BASE_MSR, a2);
            return 0;
        }
        if (a1 == ARCH_GET_FS) return user_store_u64(a2, fs_base) ? 0 : -LINUX_EFAULT;
        if (a1 == ARCH_SET_GS) return a2 == 0 ? 0 : -LINUX_EPERM;
        if (a1 == ARCH_GET_GS) return user_store_u64(a2, 0) ? 0 : -LINUX_EFAULT;
        return -LINUX_EINVAL;
    case SYS_SET_TID_ADDRESS: return 1;
    case SYS_SET_ROBUST_LIST: return 0;
    case SYS_GETPID:
    case SYS_GETTID: return 1;
    case SYS_GETPPID: return 0;
    case SYS_GETUID:
    case SYS_GETGID:
    case SYS_GETEUID:
    case SYS_GETEGID: return 0;
    case SYS_GETGROUPS: return a1 == 0 ? 0 : 0;
    case SYS_GETRESUID:
    case SYS_GETRESGID:
        if (a1 && !user_store_u32(a1, 0)) return -LINUX_EFAULT;
        if (a2 && !user_store_u32(a2, 0)) return -LINUX_EFAULT;
        if (a3 && !user_store_u32(a3, 0)) return -LINUX_EFAULT;
        return 0;
    case SYS_BRK: return sys_brk(a1);
    case SYS_MMAP: return sys_mmap(a1, a2, a3, a4, a5, a6);
    case SYS_MPROTECT: return sys_mprotect(a1, a2, a3);
    case SYS_MUNMAP: return sys_munmap(a1, a2);
    case SYS_MADVISE: return 0;
    case SYS_RT_SIGACTION:
        if (a3 != 0 && !user_zero(a3, 32)) return -LINUX_EFAULT;
        return 0;
    case SYS_RT_SIGPROCMASK:
        if (a4 > 128) return -LINUX_EINVAL;
        if (a3 != 0 && !user_zero(a3, a4)) return -LINUX_EFAULT;
        return 0;
    case SYS_SIGALTSTACK:
        if (a2 != 0 && !user_zero(a2, 24)) return -LINUX_EFAULT;
        return 0;
    case SYS_PRLIMIT64:
    case SYS_GETRLIMIT: {
        const uint64_t out = number == SYS_PRLIMIT64 ? a4 : a2;
        if (out != 0) {
            uint64_t limits[2] = { UINT64_MAX, UINT64_MAX };
            if (!user_copy_out(out, limits, sizeof(limits))) return -LINUX_EFAULT;
        }
        return 0;
    }
    case SYS_GETRANDOM:
        if (!user_range(a1, a2, true)) return -LINUX_EFAULT;
        for (uint64_t i = 0; i < a2; ++i) {
            random_state = random_state * 6364136223846793005ull + 1ull;
            uint8_t value = (uint8_t)(random_state >> 32);
            if (!user_copy_out(a1 + i, &value, 1)) return -LINUX_EFAULT;
        }
        return (int64_t)a2;
    case SYS_CLOCK_GETTIME:
    case SYS_GETTIMEOFDAY:
        if (!user_zero(number == SYS_CLOCK_GETTIME ? a2 : a1, 16)) return -LINUX_EFAULT;
        return 0;
    case SYS_NANOSLEEP:
        if (a2 != 0 && !user_zero(a2, 16)) return -LINUX_EFAULT;
        return 0;
    case SYS_FSTAT:
        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;
        return fill_stat(a2, (int)a1 == 4 ? (S_IFCHR | 0666u) : (S_IFCHR | 0666u));
    case SYS_STAT:
    case SYS_LSTAT: return stat_path(a1, a2);
    case SYS_NEWFSTATAT: return stat_path(a2, a3);
    case SYS_IOCTL: return sys_ioctl(a1, a2, a3);
    case SYS_FCNTL: return sys_fcntl((int)a1, a2, a3);
    case SYS_POLL: return sys_poll(a1, a2, (int64_t)a3);
    case SYS_PPOLL: return sys_poll(a1, a2, -1);
    case SYS_SELECT:
    case SYS_PSELECT6:
        /* BusyBox's line editor can use poll/read on this terminal. Returning
         * ENOSYS here lets libc fall back rather than fabricating fd_sets. */
        return -LINUX_ENOSYS;
    case SYS_CLOSE:
        return ((int)a1 >= 0 && (int)a1 <= 9) ? 0 : -LINUX_EBADF;
    case SYS_LSEEK: return -LINUX_ESPIPE;
    case SYS_DUP:
        return fd_is_tty((int)a1) ? 3 : -LINUX_EBADF;
    case SYS_DUP2:
    case SYS_DUP3:
        return fd_is_tty((int)a1) && (int)a2 >= 0 && (int)a2 <= 9 ? (int64_t)a2 : -LINUX_EBADF;
    case SYS_OPEN: return sys_open_path(a1);
    case SYS_OPENAT: return sys_open_path(a2);
    case SYS_ACCESS: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        return path_is_known(path) ? 0 : -LINUX_ENOENT;
    }
    case SYS_GETCWD: {
        const size_t length = string_length(current_directory) + 1u;
        if (a2 < length || !user_copy_out(a1, current_directory, length)) return -LINUX_EFAULT;
        return (int64_t)length;
    }
    case SYS_CHDIR: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/") && !string_equal(path, ".")) return -LINUX_ENOENT;
        current_directory[0] = '/'; current_directory[1] = '\0';
        return 0;
    }
    case SYS_READLINK: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/busybox";
        size_t length = sizeof(target) - 1u;
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
    case SYS_UMASK: return 0022;
    case SYS_UNAME: return sys_uname(a1);
    case SYS_SETPGID:
        foreground_pgrp = 1; return 0;
    case SYS_GETPGRP:
    case SYS_GETPGID:
    case SYS_GETSID: return 1;
    case SYS_SETSID: foreground_pgrp = 1; return 1;
    case SYS_KILL:
    case SYS_TGKILL: return 0;
    case SYS_PRCTL: return 0;
    case SYS_FUTEX: return 0;
    case SYS_RSEQ: return -LINUX_ENOSYS;
    case SYS_WAIT4: return -LINUX_ECHILD;
    case SYS_GETDENTS64: return 0;
    case SYS_PIPE2:
        return -LINUX_ENOSYS;
    default:
        log_unknown(number);
        return -LINUX_ENOSYS;
    }
}

/* user.S calls this for every BusyBox build. It preserves the already-working
 * echo regression test when the shell is not active, and switches to the richer
 * interactive ABI only while this second BusyBox instance owns the terminal. */
int64_t linux_busybox_syscall_router(uint64_t number,
                                     uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    if (shell_active) return shell_dispatch(number, a1, a2, a3, a4, a5, a6);
    return linux_busybox_syscall_dispatch(number, a1, a2, a3, a4, a5, a6);
}

static int busybox_shell_init(void) {
    trace("preparing official unmodified BusyBox ash interactive session");
    if (!gdt_is_initialized() || vmm_kernel_space() == VMM_INVALID_SPACE) {
        trace("GDT/VMM unavailable");
        return 0;
    }
    if (!syscall_init()) {
        trace("SYSCALL/SYSRET setup failed");
        return 0;
    }
    if (!load_shell()) {
        trace("BusyBox ELF loader rejected shell instance");
        return 0;
    }

    uint64_t translated = 0, flags = 0;
    if (!vmm_translate(image.space, (uint64_t)(uintptr_t)&linux_syscall_entry,
                       &translated, &flags)) {
        trace("LSTAR entry missing from ash process CR3");
        destroy_image();
        return 0;
    }

    init_termios();
    tty_set_active(true);
    /* ps2_keyboard_init() has received the device ACK by the time Linux module
     * initcalls run, but entry.c normally unmasks IRQ1 after the runtime returns.
     * Interactive ash needs IRQ1 now, so expose it for the duration of the shell. */
    pic_unmask_irq(1);

    uint64_t rflags = 0;
    __asm__ volatile ("pushfq; popq %0" : "=r"(rflags));
    const bool interrupts_were_enabled = (rflags & (1ull << 9)) != 0;

    shell_active = true;
    shell_exit_seen = false;
    shell_exit_status = -1;
    trace("entering UNMODIFIED BusyBox /bin/sh -i at CPL3; type in serial or PS/2");
    tty_write("\nTwilight BusyBox shell — built-ins work now; type 'exit' to return to kernel.\n", 79);
    user_mode_enter_interruptible(image.entry, image.stack_pointer,
                                  image.space, vmm_kernel_space());
    shell_active = false;
    tty_set_active(false);

    if (interrupts_were_enabled) __asm__ volatile ("sti" ::: "memory");
    else __asm__ volatile ("cli" ::: "memory");

    if (shell_exit_seen)
        trace(shell_exit_status == 0 ?
              "interactive unmodified BusyBox ash exited cleanly" :
              "interactive BusyBox ash exited with nonzero status");
    else
        trace("ash returned to kernel without exit/exit_group observation");

    destroy_image();
    return 0;
}

module_init(busybox_shell_init);

#endif
