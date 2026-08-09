#!/usr/bin/env python3
"""Inject the first real rootfs execve path into the generated Plasma Bash ABI.

This is deliberately scoped to the Plasma bring-up build.  It teaches the
single-process Linux ABI shim to replace the current image with an ELF from the
Limine CPIO rootfs, including the PT_INTERP image and a Linux initial stack.
That lets Bash's `exec` builtin cross directly into Alpine/musl before Twilight
has a scheduler/fork implementation.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected generated source fragment not found: {old[:120]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#define SYS_GETPID          39ull\n",
        "#define SYS_GETPID          39ull\n#define SYS_EXECVE          59ull\n",
    )
    text = replace_once(
        text,
        "#define LINUX_ENOENT      2\n",
        "#define LINUX_ENOENT      2\n#define LINUX_E2BIG       7\n#define LINUX_ENOEXEC     8\n",
    )
    text = replace_once(
        text,
        "#define ET_EXEC 2u\n",
        "#define ET_EXEC 2u\n#define ET_DYN  3u\n",
    )
    text = replace_once(
        text,
        "#define PT_LOAD 1u\n",
        "#define PT_LOAD 1u\n#define PT_INTERP 3u\n",
    )
    text = replace_once(
        text,
        "extern void linux_syscall_entry(void);\n",
        "extern void linux_syscall_entry(void);\n"
        "extern void linux_exec_set_return_state(uint64_t instruction_pointer,\n"
        "                                        uint64_t stack_pointer);\n",
    )

    state_anchor = "#define ROOTFS_FD_FIRST 10\n"
    state = r'''#define PLASMA_EXEC_MAX_ARGS 32u
#define PLASMA_EXEC_MAX_ENV 64u
#define PLASMA_EXEC_STRING 256u
#define PLASMA_MAIN_BIAS   0x0000000000400000ull
#define PLASMA_INTERP_BIAS 0x0000000200000000ull

static char plasma_exec_path[PLASMA_EXEC_STRING] = "/bin/bash";
static char plasma_exec_args[PLASMA_EXEC_MAX_ARGS][PLASMA_EXEC_STRING];
static char plasma_exec_env[PLASMA_EXEC_MAX_ENV][PLASMA_EXEC_STRING];
static size_t plasma_exec_argc;
static size_t plasma_exec_envc;

#define ROOTFS_FD_FIRST 10
'''
    text = replace_once(text, state_anchor, state)

    exec_code = r'''
struct plasma_loaded_elf {
    struct elf64_ehdr eh;
    uint64_t bias;
    uint64_t entry;
    uint64_t phdr;
    uint64_t max_end;
};

static bool plasma_elf_header(const struct rootfs_node *node, struct elf64_ehdr *eh) {
    if (node == 0 || eh == 0 || node->size < sizeof(*eh)) return false;
    bytes_copy(eh, node->data, sizeof(*eh));
    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L' ||
        eh->ident[3] != 'F' || eh->ident[4] != ELFCLASS64 ||
        eh->ident[5] != ELFDATA2LSB || eh->ident[6] != EV_CURRENT ||
        (eh->type != ET_EXEC && eh->type != ET_DYN) ||
        eh->machine != EM_X86_64 || eh->version != EV_CURRENT ||
        eh->ehsize != sizeof(*eh) || eh->phentsize != sizeof(struct elf64_phdr) ||
        eh->phnum == 0 || eh->phoff > node->size ||
        (uint64_t)eh->phnum > (node->size - eh->phoff) / sizeof(struct elf64_phdr))
        return false;
    return true;
}

static bool plasma_read_phdr(const struct rootfs_node *node,
                             const struct elf64_ehdr *eh,
                             uint16_t index,
                             struct elf64_phdr *out) {
    if (node == 0 || eh == 0 || out == 0 || index >= eh->phnum) return false;
    const uint64_t offset = eh->phoff + (uint64_t)index * sizeof(*out);
    if (offset > node->size || sizeof(*out) > node->size - offset) return false;
    bytes_copy(out, node->data + offset, sizeof(*out));
    return true;
}

static bool plasma_interp_path(const struct rootfs_node *node,
                               const struct elf64_ehdr *eh,
                               char out[PLASMA_EXEC_STRING]) {
    for (uint16_t i = 0; i < eh->phnum; ++i) {
        struct elf64_phdr ph;
        if (!plasma_read_phdr(node, eh, i, &ph)) return false;
        if (ph.type != PT_INTERP) continue;
        if (ph.filesz < 2u || ph.filesz >= PLASMA_EXEC_STRING ||
            ph.offset > node->size || ph.filesz > node->size - ph.offset)
            return false;
        bytes_copy(out, node->data + ph.offset, (size_t)ph.filesz);
        out[ph.filesz - 1u] = '\0';
        return true;
    }
    out[0] = '\0';
    return true;
}

static int64_t plasma_capture_vector(uint64_t vector_address,
                                     char storage[][PLASMA_EXEC_STRING],
                                     size_t limit,
                                     size_t *count_out) {
    if (count_out == 0) return -LINUX_EFAULT;
    *count_out = 0;
    if (vector_address == 0) return 0;
    for (size_t i = 0; i < limit; ++i) {
        uint64_t string_address = 0;
        if (!user_copy_in(&string_address,
                          vector_address + i * sizeof(uint64_t),
                          sizeof(string_address)))
            return -LINUX_EFAULT;
        if (string_address == 0) {
            *count_out = i;
            return 0;
        }
        if (!copy_user_string(string_address, storage[i], PLASMA_EXEC_STRING))
            return -LINUX_E2BIG;
    }
    return -LINUX_E2BIG;
}

static void plasma_copy_kernel_string(char *out, size_t capacity, const char *in) {
    if (out == 0 || capacity == 0) return;
    size_t i = 0;
    if (in != 0) {
        while (i + 1u < capacity && in[i] != '\0') {
            out[i] = in[i];
            ++i;
        }
    }
    out[i] = '\0';
}

static void plasma_discard_old_user_pages(void) {
    write_msr(IA32_FS_BASE_MSR, 0);
    fs_base = 0;
    for (size_t i = 0; i < image.page_count; ++i) {
        uint64_t old_phys = 0;
        if (vmm_unmap_page(image.space, image.pages[i].va, &old_phys) && old_phys != 0)
            (void)pmm_free_page(old_phys);
    }
    image.page_count = 0;
    image.entry = 0;
    image.stack_pointer = 0;
    image.phdr = 0;
    image.phnum = 0;
    image.phentsize = 0;
    image.brk_base = 0;
    image.brk_current = 0;
    image.mmap_next = SHELL_MMAP_BASE;
}

static bool plasma_map_elf(const struct rootfs_node *node,
                           uint64_t dyn_bias,
                           struct plasma_loaded_elf *loaded) {
    if (loaded == 0 || !plasma_elf_header(node, &loaded->eh)) return false;
    loaded->bias = loaded->eh.type == ET_DYN ? dyn_bias : 0;
    loaded->entry = loaded->bias + loaded->eh.entry;
    loaded->phdr = 0;
    loaded->max_end = 0;
    bool entry_covered = false;

    for (uint16_t index = 0; index < loaded->eh.phnum; ++index) {
        struct elf64_phdr ph;
        if (!plasma_read_phdr(node, &loaded->eh, index, &ph)) return false;
        if (ph.type == PT_PHDR) loaded->phdr = loaded->bias + ph.vaddr;
        if (ph.type != PT_LOAD || ph.memsz == 0) continue;
        if (ph.filesz > ph.memsz || ph.offset > node->size ||
            ph.filesz > node->size - ph.offset ||
            ph.vaddr > UINT64_MAX - loaded->bias ||
            loaded->bias + ph.vaddr >= SHELL_USER_TOP ||
            ph.memsz > SHELL_USER_TOP - (loaded->bias + ph.vaddr))
            return false;

        const uint64_t segment_va = loaded->bias + ph.vaddr;
        const uint64_t end = segment_va + ph.memsz;
        if (end > loaded->max_end) loaded->max_end = end;
        if (loaded->entry >= segment_va && loaded->entry < end && (ph.flags & PF_X) != 0)
            entry_covered = true;
        if (loaded->phdr == 0 && loaded->eh.phoff >= ph.offset &&
            loaded->eh.phoff + (uint64_t)loaded->eh.phnum * loaded->eh.phentsize <=
                ph.offset + ph.filesz)
            loaded->phdr = segment_va + (loaded->eh.phoff - ph.offset);

        uint64_t page_end = 0;
        if (!align_up(end, &page_end)) return false;
        uint64_t page_flags = VMM_FLAG_USER;
        if ((ph.flags & PF_W) != 0) page_flags |= VMM_FLAG_WRITE;
        if ((ph.flags & PF_X) == 0 && vmm_nx_supported()) page_flags |= VMM_FLAG_NO_EXECUTE;
        for (uint64_t va = align_down(segment_va); va < page_end; va += TWILIGHT_PAGE_SIZE)
            if (add_page(va, page_flags) == 0) return false;
        if (ph.filesz != 0 &&
            !copy_to_process(segment_va, node->data + ph.offset, ph.filesz))
            return false;
    }
    return entry_covered && loaded->phdr != 0;
}

static bool plasma_build_exec_stack(const struct plasma_loaded_elf *main_elf,
                                    uint64_t interp_base,
                                    const char *filename) {
    const uint64_t stack_base = SHELL_STACK_TOP -
                                (uint64_t)SHELL_STACK_PAGES * TWILIGHT_PAGE_SIZE;
    uint64_t stack_flags = VMM_FLAG_USER | VMM_FLAG_WRITE;
    if (vmm_nx_supported()) stack_flags |= VMM_FLAG_NO_EXECUTE;
    for (uint64_t va = stack_base; va < SHELL_STACK_TOP; va += TWILIGHT_PAGE_SIZE)
        if (add_page(va, stack_flags) == 0) return false;

    uint64_t cursor = SHELL_STACK_TOP;
    uint64_t arg_va[PLASMA_EXEC_MAX_ARGS];
    uint64_t env_va[PLASMA_EXEC_MAX_ENV];
    bytes_zero(arg_va, sizeof(arg_va));
    bytes_zero(env_va, sizeof(env_va));

    const char platform[] = "x86_64";
    const uint8_t random_bytes[16] = {
        0x54,0x77,0x69,0x6c,0x69,0x67,0x68,0x74,
        0x50,0x6c,0x61,0x73,0x6d,0x61,0x21,0x21,
    };
    uint64_t platform_va = 0, random_va = 0, execfn_va = 0;
    if (!push_stack_bytes(&cursor, random_bytes, sizeof(random_bytes), &random_va) ||
        !push_stack_string(&cursor, platform, &platform_va) ||
        !push_stack_string(&cursor, filename, &execfn_va))
        return false;

    for (size_t i = plasma_exec_envc; i != 0; --i)
        if (!push_stack_string(&cursor, plasma_exec_env[i - 1u], &env_va[i - 1u])) return false;
    for (size_t i = plasma_exec_argc; i != 0; --i)
        if (!push_stack_string(&cursor, plasma_exec_args[i - 1u], &arg_va[i - 1u])) return false;

    const struct aux_pair aux[] = {
        { AT_PHDR, main_elf->phdr }, { AT_PHENT, main_elf->eh.phentsize },
        { AT_PHNUM, main_elf->eh.phnum }, { AT_PAGESZ, TWILIGHT_PAGE_SIZE },
        { AT_BASE, interp_base }, { AT_FLAGS, 0 }, { AT_ENTRY, main_elf->entry },
        { AT_UID, 0 }, { AT_EUID, 0 }, { AT_GID, 0 }, { AT_EGID, 0 },
        { AT_PLATFORM, platform_va }, { AT_HWCAP, 0 }, { AT_CLKTCK, 100 },
        { AT_SECURE, 0 }, { AT_RANDOM, random_va }, { AT_HWCAP2, 0 },
        { AT_EXECFN, execfn_va }, { AT_NULL, 0 },
    };

    const size_t aux_words = sizeof(aux) / sizeof(aux[0]) * 2u;
    const size_t table_words = 1u + plasma_exec_argc + 1u +
                               plasma_exec_envc + 1u + aux_words;
    const uint64_t table_bytes = (uint64_t)table_words * 8ull;
    if (cursor < stack_base + table_bytes) return false;
    uint64_t p = (cursor - table_bytes) & ~15ull;
    image.stack_pointer = p;

    if (!stack_u64(p, plasma_exec_argc)) return false;
    p += 8;
    for (size_t i = 0; i < plasma_exec_argc; ++i) {
        if (!stack_u64(p, arg_va[i])) return false;
        p += 8;
    }
    if (!stack_u64(p, 0)) return false;
    p += 8;
    for (size_t i = 0; i < plasma_exec_envc; ++i) {
        if (!stack_u64(p, env_va[i])) return false;
        p += 8;
    }
    if (!stack_u64(p, 0)) return false;
    p += 8;
    for (size_t i = 0; i < sizeof(aux) / sizeof(aux[0]); ++i) {
        if (!stack_u64(p, aux[i].type)) return false;
        p += 8;
        if (!stack_u64(p, aux[i].value)) return false;
        p += 8;
    }
    return true;
}

static int64_t plasma_execve(uint64_t filename_address,
                             uint64_t argv_address,
                             uint64_t envp_address) {
    char filename[PLASMA_EXEC_STRING];
    if (!copy_user_string(filename_address, filename, sizeof(filename))) return -LINUX_EFAULT;

    struct rootfs_node executable;
    if (!rootfs_lookup_follow(filename, &executable)) return -LINUX_ENOENT;
    struct elf64_ehdr main_header;
    if (!plasma_elf_header(&executable, &main_header)) return -LINUX_ENOEXEC;

    char interpreter_path[PLASMA_EXEC_STRING];
    if (!plasma_interp_path(&executable, &main_header, interpreter_path)) return -LINUX_ENOEXEC;
    struct rootfs_node interpreter;
    bool has_interpreter = interpreter_path[0] != '\0';
    if (has_interpreter && !rootfs_lookup_follow(interpreter_path, &interpreter))
        return -LINUX_ENOENT;
    if (has_interpreter) {
        struct elf64_ehdr interp_header;
        if (!plasma_elf_header(&interpreter, &interp_header)) return -LINUX_ENOEXEC;
    }

    int64_t rc = plasma_capture_vector(argv_address, plasma_exec_args,
                                       PLASMA_EXEC_MAX_ARGS, &plasma_exec_argc);
    if (rc != 0) return rc;
    rc = plasma_capture_vector(envp_address, plasma_exec_env,
                               PLASMA_EXEC_MAX_ENV, &plasma_exec_envc);
    if (rc != 0) return rc;
    if (plasma_exec_argc == 0) {
        plasma_copy_kernel_string(plasma_exec_args[0], PLASMA_EXEC_STRING, filename);
        plasma_exec_argc = 1;
    }

    plasma_copy_kernel_string(plasma_exec_path, sizeof(plasma_exec_path), filename);
    plasma_discard_old_user_pages();

    struct plasma_loaded_elf main_elf;
    if (!plasma_map_elf(&executable, PLASMA_MAIN_BIAS, &main_elf)) return -LINUX_ENOEXEC;

    uint64_t start_entry = main_elf.entry;
    uint64_t interp_base = 0;
    if (has_interpreter) {
        struct plasma_loaded_elf interp_elf;
        if (!plasma_map_elf(&interpreter, PLASMA_INTERP_BIAS, &interp_elf))
            return -LINUX_ENOEXEC;
        start_entry = interp_elf.entry;
        interp_base = interp_elf.bias;
    }

    image.entry = start_entry;
    image.phdr = main_elf.phdr;
    image.phnum = main_elf.eh.phnum;
    image.phentsize = main_elf.eh.phentsize;
    if (!align_up(main_elf.max_end, &image.brk_base)) return -LINUX_ENOMEM;
    image.brk_current = image.brk_base;
    image.mmap_next = SHELL_MMAP_BASE;

    if (!plasma_build_exec_stack(&main_elf, interp_base, filename)) return -LINUX_ENOMEM;
    for (size_t i = 0; i < image.page_count; ++i)
        if (!protect_page(&image.pages[i])) return -LINUX_EACCES;

    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));
    linux_exec_set_return_state(image.entry, image.stack_pointer);
    serial_write("[linux:plasma] execve switched image to ");
    serial_write(filename);
    serial_write(has_interpreter ? " through musl\n" : " directly\n");
    return 0;
}

'''
    text = replace_once(text, "static bool map_runtime_page(uint64_t va, uint64_t flags) {\n",
                        exec_code + "static bool map_runtime_page(uint64_t va, uint64_t flags) {\n")

    text = replace_once(
        text,
        "    case SYS_EXIT:\n",
        "    case SYS_EXECVE: return plasma_execve(a1, a2, a3);\n    case SYS_EXIT:\n",
    )

    # Let musl/BusyBox discover the post-exec image through /proc/self/exe.
    old_readlink = '''        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
'''
    new_readlink = '''        size_t length = string_length(plasma_exec_path);
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, plasma_exec_path, length) ? (int64_t)length : -LINUX_EFAULT;
'''
    if old_readlink in text:
        text = text.replace(old_readlink, new_readlink, 1)

    old_readlinkat = '''        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a4) length = (size_t)a4;
        return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
'''
    new_readlinkat = '''        size_t length = string_length(plasma_exec_path);
        if (length > a4) length = (size_t)a4;
        return user_copy_out(a3, plasma_exec_path, length) ? (int64_t)length : -LINUX_EFAULT;
'''
    if old_readlinkat in text:
        text = text.replace(old_readlinkat, new_readlinkat, 1)

    path.write_text(text, encoding="utf-8")
    print(f"Added Plasma rootfs execve + PT_INTERP loader path: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
