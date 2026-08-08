#!/usr/bin/env python3
"""Inject read-only Twilight rootfs file descriptors into generated Bash ABI C.

Keeping this as a second source transformation avoids duplicating the large,
already-proven Bash/Linux ABI shim while Plasma bring-up is still moving fast.
The underlying rootfs API is generic and will later be used by exec/dynamic ELF.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected generated Bash source fragment not found: {old[:120]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        '#include <twilight/vmm.h>\n',
        '#include <twilight/vmm.h>\n#include <twilight/rootfs.h>\n',
    )

    state_anchor = 'static int foreground_pgrp = 1;\n'
    rootfs_state = r'''static int foreground_pgrp = 1;

#define ROOTFS_FD_FIRST 10
#define ROOTFS_FD_COUNT 48

struct rootfs_open_file {
    bool used;
    struct rootfs_node node;
    uint64_t offset;
};

static struct rootfs_open_file rootfs_open_files[ROOTFS_FD_COUNT];

static struct rootfs_open_file *rootfs_file_for_fd(int fd) {
    if (fd < ROOTFS_FD_FIRST || fd >= ROOTFS_FD_FIRST + ROOTFS_FD_COUNT)
        return 0;
    struct rootfs_open_file *file = &rootfs_open_files[fd - ROOTFS_FD_FIRST];
    return file->used ? file : 0;
}

static int rootfs_allocate_fd(const struct rootfs_node *node) {
    if (node == 0) return -LINUX_EINVAL;
    for (int i = 0; i < ROOTFS_FD_COUNT; ++i) {
        if (rootfs_open_files[i].used) continue;
        rootfs_open_files[i].used = true;
        rootfs_open_files[i].node = *node;
        rootfs_open_files[i].offset = 0;
        return ROOTFS_FD_FIRST + i;
    }
    return -LINUX_EBUSY;
}

static int64_t rootfs_close_fd(int fd) {
    struct rootfs_open_file *file = rootfs_file_for_fd(fd);
    if (file == 0) return -LINUX_EBADF;
    *file = (struct rootfs_open_file){0};
    return 0;
}

static int64_t rootfs_read_fd(int fd, uint64_t address, uint64_t length) {
    struct rootfs_open_file *file = rootfs_file_for_fd(fd);
    if (file == 0) return -LINUX_EBADF;
    if (length == 0) return 0;
    if (!user_range(address, length, true)) return -LINUX_EFAULT;

    uint8_t buffer[256];
    uint64_t done = 0;
    while (done < length) {
        size_t chunk = (size_t)(length - done);
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);
        const size_t got = rootfs_read(&file->node, (size_t)file->offset,
                                       buffer, chunk);
        if (got == 0) break;
        if (!user_copy_out(address + done, buffer, got)) return -LINUX_EFAULT;
        file->offset += got;
        done += got;
        if (got < chunk) break;
    }
    return (int64_t)done;
}

static int64_t rootfs_lseek_fd(int fd, int64_t offset, int whence) {
    struct rootfs_open_file *file = rootfs_file_for_fd(fd);
    if (file == 0) return -LINUX_EBADF;

    int64_t base;
    switch (whence) {
    case 0: base = 0; break;                 /* SEEK_SET */
    case 1: base = (int64_t)file->offset; break; /* SEEK_CUR */
    case 2: base = (int64_t)file->node.size; break; /* SEEK_END */
    default: return -LINUX_EINVAL;
    }
    if ((offset < 0 && base < -offset) ||
        (offset > 0 && base > INT64_MAX - offset)) return -LINUX_EINVAL;
    const int64_t next = base + offset;
    if (next < 0) return -LINUX_EINVAL;
    file->offset = (uint64_t)next;
    return next;
}
'''
    text = replace_once(text, state_anchor, rootfs_state)

    old_paths = r'''static bool path_is_known(const char *path) {
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
'''
    new_paths = r'''static int64_t fill_rootfs_stat(uint64_t address, const struct rootfs_node *node) {
    if (node == 0) return -LINUX_EINVAL;
    int64_t rc = fill_stat(address, node->mode);
    if (rc != 0) return rc;
    if (!user_store_u64(address + 48, node->size)) return -LINUX_EFAULT; /* st_size */
    if (!user_store_u64(address + 56, 4096)) return -LINUX_EFAULT;      /* st_blksize */
    const uint64_t blocks = (node->size + 511u) / 512u;
    if (!user_store_u64(address + 64, blocks)) return -LINUX_EFAULT;    /* st_blocks */
    return 0;
}

static bool path_is_known(const char *path) {
    if (string_equal(path, "/") || string_equal(path, ".") ||
        string_equal(path, "/dev/tty") || string_equal(path, "/dev/null") ||
        string_equal(path, "/bin/sh") || string_equal(path, "/bin/busybox") ||
        string_equal(path, "/bin/bash")) return true;
    struct rootfs_node node;
    return rootfs_available() && rootfs_lookup(path, &node);
}

static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (string_equal(path, "/dev/tty") || string_equal(path, "/dev/null"))
        return fill_stat(stat_address, S_IFCHR | 0666u);
    if (string_equal(path, "/bin/sh") || string_equal(path, "/bin/busybox") ||
        string_equal(path, "/bin/bash"))
        return fill_stat(stat_address, S_IFREG | 0755u);

    struct rootfs_node node;
    if (rootfs_available() && rootfs_lookup(path, &node))
        return fill_rootfs_stat(stat_address, &node);
    if (string_equal(path, "/") || string_equal(path, "."))
        return fill_stat(stat_address, S_IFDIR | 0755u);
    return -LINUX_ENOENT;
}

static int64_t sys_open_path(uint64_t path_address) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (string_equal(path, "/dev/tty")) return 3;
    if (string_equal(path, "/dev/null")) return 4;

    struct rootfs_node node;
    if (rootfs_available() && rootfs_lookup(path, &node))
        return rootfs_allocate_fd(&node);
    return -LINUX_ENOENT;
}
'''
    text = replace_once(text, old_paths, new_paths)

    text = replace_once(
        text,
        '''    case SYS_READ:
        if ((int)a1 == 4) return 0; /* /dev/null */
        return sys_read_tty((int)a1, a2, a3);
''',
        '''    case SYS_READ:
        if ((int)a1 == 4) return 0; /* /dev/null */
        if (rootfs_file_for_fd((int)a1) != 0) return rootfs_read_fd((int)a1, a2, a3);
        return sys_read_tty((int)a1, a2, a3);
''',
    )

    text = replace_once(
        text,
        '''    case SYS_FSTAT:
        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;
        return fill_stat(a2, (int)a1 == 4 ? (S_IFCHR | 0666u) : (S_IFCHR | 0666u));
''',
        '''    case SYS_FSTAT: {
        struct rootfs_open_file *file = rootfs_file_for_fd((int)a1);
        if (file != 0) return fill_rootfs_stat(a2, &file->node);
        if (!fd_is_tty((int)a1) && (int)a1 != 4) return -LINUX_EBADF;
        return fill_stat(a2, S_IFCHR | 0666u);
    }
''',
    )

    text = replace_once(
        text,
        '''    case SYS_CLOSE:
        return ((int)a1 >= 0 && (int)a1 <= 9) ? 0 : -LINUX_EBADF;
    case SYS_LSEEK: return -LINUX_ESPIPE;
''',
        '''    case SYS_CLOSE:
        if (rootfs_file_for_fd((int)a1) != 0) return rootfs_close_fd((int)a1);
        return ((int)a1 >= 0 && (int)a1 <= 9) ? 0 : -LINUX_EBADF;
    case SYS_LSEEK:
        if (rootfs_file_for_fd((int)a1) != 0)
            return rootfs_lseek_fd((int)a1, (int64_t)a2, (int)a3);
        return -LINUX_ESPIPE;
''',
    )

    # Make rootfs descriptor state deterministic for every shell launch.
    text = replace_once(
        text,
        '    init_termios();\n',
        '    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n    init_termios();\n',
    )

    path.write_text(text, encoding="utf-8")
    print(f"Added read-only rootfs file descriptors to Bash compatibility unit: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
