#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/printk.h>
#include <twilight/rootfs.h>

#define ELFCLASS64 2u
#define ELFDATA2LSB 1u
#define EM_X86_64 62u
#define PT_INTERP 3u
#define ROOTFS_MODE_TYPE_MASK 0170000u
#define ROOTFS_MODE_REGULAR   0100000u

struct __attribute__((packed)) elf64_ehdr {
    uint8_t ident[16];
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

static bool copy_bytes(void *destination, const void *source, size_t size) {
    if (destination == 0 || source == 0) return false;
    uint8_t *out = destination;
    const uint8_t *in = source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return true;
}

static bool elf_header(const struct rootfs_node *node, struct elf64_ehdr *eh) {
    if (node == 0 || eh == 0 || node->size < sizeof(*eh) ||
        (node->mode & ROOTFS_MODE_TYPE_MASK) != ROOTFS_MODE_REGULAR)
        return false;
    if (!copy_bytes(eh, node->data, sizeof(*eh))) return false;
    return eh->ident[0] == 0x7f && eh->ident[1] == 'E' && eh->ident[2] == 'L' &&
           eh->ident[3] == 'F' && eh->ident[4] == ELFCLASS64 &&
           eh->ident[5] == ELFDATA2LSB && eh->machine == EM_X86_64 &&
           eh->ehsize == sizeof(*eh) && eh->phentsize == sizeof(struct elf64_phdr) &&
           eh->phnum != 0 && eh->phoff <= node->size &&
           (uint64_t)eh->phnum <= (node->size - eh->phoff) / sizeof(struct elf64_phdr);
}

static bool read_interpreter(const struct rootfs_node *node,
                             const struct elf64_ehdr *eh,
                             char out[192]) {
    if (node == 0 || eh == 0 || out == 0) return false;
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        struct elf64_phdr ph;
        const size_t off = (size_t)eh->phoff + (size_t)i * sizeof(ph);
        if (off > node->size || sizeof(ph) > node->size - off) return false;
        copy_bytes(&ph, node->data + off, sizeof(ph));
        if (ph.type != PT_INTERP) continue;
        if (ph.filesz < 2u || ph.filesz >= 192u || ph.offset > node->size ||
            ph.filesz > node->size - ph.offset) return false;
        copy_bytes(out, node->data + ph.offset, (size_t)ph.filesz);
        out[ph.filesz - 1u] = '\0';
        return true;
    }
    return false;
}

void plasma_elf_probe(void) {
    struct rootfs_node executable;
    if (!rootfs_lookup_follow("/bin/busybox", &executable)) {
        pr_warn("Plasma ELF gate: /bin/busybox not present in rootfs");
        return;
    }

    struct elf64_ehdr eh;
    if (!elf_header(&executable, &eh)) {
        pr_err("Plasma ELF gate: /bin/busybox is not a supported ELF64 x86_64 image");
        return;
    }

    printk("[linux] Plasma ELF gate: /bin/busybox type=%u entry=%#llx phnum=%u size=%zu",
           (unsigned)eh.type, (unsigned long long)eh.entry,
           (unsigned)eh.phnum, executable.size);

    char interpreter[192];
    if (!read_interpreter(&executable, &eh, interpreter)) {
        printk("[linux] Plasma ELF gate: /bin/busybox has no PT_INTERP (static/direct exec path)");
        return;
    }

    printk("[linux] Plasma ELF gate: PT_INTERP=%s", interpreter);

    struct rootfs_node loader;
    if (!rootfs_lookup_follow(interpreter, &loader)) {
        pr_err("Plasma ELF gate: PT_INTERP target is missing after symlink resolution");
        return;
    }

    struct elf64_ehdr loader_eh;
    if (!elf_header(&loader, &loader_eh)) {
        pr_err("Plasma ELF gate: musl interpreter is not a supported ELF64 x86_64 image");
        return;
    }

    printk("[linux] Plasma ELF gate PASS: loader=%s type=%u entry=%#llx size=%zu",
           loader.name, (unsigned)loader_eh.type,
           (unsigned long long)loader_eh.entry, loader.size);
    printk("[linux] Plasma next gate: execve can now target this rootfs ELF + musl pair");
}
