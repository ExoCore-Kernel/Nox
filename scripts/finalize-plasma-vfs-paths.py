#!/usr/bin/env python3
"""Implement Linux dirfd-relative path and symlink semantics used by Qt/KDE.

This finalizer replaces the early cwd-only openat/newfstatat/statx/readlinkat
shortcuts with real semantics for the filesystems Twilight currently exposes:

* absolute paths ignore dirfd, as Linux requires;
* relative *at() paths resolve against AT_FDCWD or an open directory fd;
* invalid/non-directory dirfds return EBADF/ENOTDIR;
* rootfs lookup resolves symlinks in every pathname component, with a Linux-like
  40-link traversal limit, instead of following only the final CPIO entry;
* stat/lstat/newfstatat/statx honor final-component symlink following;
* readlink/readlinkat return actual CPIO symlink payloads;
* /proc/self/exe reports the scheduler's current exec path rather than always
  claiming that every KDE process is /bin/bash;
* open/openat use the same resolved path implementation for rootfs, runtime VFS,
  tty/null and fbdev nodes.

This is not a claim of a complete Linux VFS.  It implements the real pathname
semantics for the concrete rootfs/runtime filesystems present in the Plasma
bring-up environment instead of returning synthetic success.
"""
from __future__ import annotations

import pathlib
import re
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected VFS path fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Linux path-resolution errors used by the implementation below.
    if "#define LINUX_ENAMETOOLONG" not in text:
        text = rep(
            text,
            "#define LINUX_ENOSYS     38\n",
            "#define LINUX_ENAMETOOLONG 36\n"
            "#define LINUX_ENOSYS     38\n"
            "#define LINUX_ELOOP      40\n",
        )

    text = rep(
        text,
        "#define PLASMA_AT_EMPTY_PATH 0x1000u\n",
        "#define PLASMA_AT_SYMLINK_NOFOLLOW 0x0100u\n"
        "#define PLASMA_AT_NO_AUTOMOUNT      0x0800u\n"
        "#define PLASMA_AT_EMPTY_PATH         0x1000u\n",
    )

    helpers = r'''
#define PLASMA_PATH_RESOLVE_MAX 512u
#define PLASMA_SYMLINK_FOLLOW_MAX 40u

static int64_t plasma_dirfd_base_path(int dirfd, char *out, size_t capacity) {
    if (out == 0 || capacity < 2u) return -LINUX_EFAULT;
    if (dirfd == PLASMA_AT_FDCWD) {
        const size_t n = string_length(current_directory);
        if (n + 1u > capacity) return -LINUX_ENAMETOOLONG;
        bytes_copy(out, current_directory, n + 1u);
        return 0;
    }

    struct rootfs_open_file *root = rootfs_file_for_fd(dirfd);
    if (root != 0) {
        if ((root->node.mode & 0170000u) != S_IFDIR) return -LINUX_ENOTDIR;
        const char *name = plasma_archive_name(root->node.name);
        if (name == 0 || name[0] == '\0' || (name[0] == '.' && name[1] == '\0')) {
            out[0] = '/'; out[1] = '\0';
            return 0;
        }
        const size_t n = string_length(name);
        if (n + 2u > capacity) return -LINUX_ENAMETOOLONG;
        out[0] = '/';
        bytes_copy(out + 1u, name, n + 1u);
        return 0;
    }

    struct plasma_runtime_fd *runtime = plasma_runtime_fd(dirfd);
    if (runtime != 0) {
        if (runtime->type != PLASMA_RT_DIR) return -LINUX_ENOTDIR;
        if (runtime->object >= PLASMA_TMP_NODES || !plasma_tmp_nodes[runtime->object].used)
            return -LINUX_EBADF;
        const char *base = plasma_tmp_nodes[runtime->object].path;
        const size_t n = string_length(base);
        if (n + 1u > capacity) return -LINUX_ENAMETOOLONG;
        bytes_copy(out, base, n + 1u);
        return 0;
    }

    return -LINUX_EBADF;
}

static int64_t plasma_resolve_at_path(int dirfd, const char *input,
                                      char *out, size_t capacity) {
    if (input == 0 || out == 0 || capacity < 2u) return -LINUX_EFAULT;
    if (input[0] == '\0') return -LINUX_ENOENT;

    if (input[0] == '/' || dirfd == PLASMA_AT_FDCWD) {
        if (!plasma_resolve_path(input, out, capacity)) return -LINUX_ENAMETOOLONG;
        return 0;
    }

    char base[PLASMA_PATH_RESOLVE_MAX];
    int64_t rc = plasma_dirfd_base_path(dirfd, base, sizeof(base));
    if (rc != 0) return rc;

    char joined[PLASMA_PATH_RESOLVE_MAX];
    const size_t base_n = string_length(base);
    const size_t input_n = string_length(input);
    size_t n = 0;
    if (base_n + input_n + 2u > sizeof(joined)) return -LINUX_ENAMETOOLONG;
    bytes_copy(joined, base, base_n);
    n = base_n;
    if (n == 0 || joined[n - 1u] != '/') joined[n++] = '/';
    bytes_copy(joined + n, input, input_n + 1u);
    if (!plasma_resolve_path(joined, out, capacity)) return -LINUX_ENAMETOOLONG;
    return 0;
}

/* Resolve every CPIO symlink component.  rootfs_lookup_follow() historically
 * handled only a symlink in the final component, which is insufficient for
 * normal Linux pathname walking. */
static int64_t plasma_rootfs_lookup_linux(const char *input,
                                          bool follow_final,
                                          struct rootfs_node *out) {
    if (input == 0 || out == 0 || !rootfs_available()) return -LINUX_ENOENT;

    char current[PLASMA_PATH_RESOLVE_MAX];
    if (!plasma_resolve_path(input, current, sizeof(current)))
        return -LINUX_ENAMETOOLONG;

    unsigned followed = 0;
    for (;;) {
        const char *scan = current;
        while (*scan == '/') ++scan;
        if (*scan == '\0')
            return rootfs_lookup("/", out) ? 0 : -LINUX_ENOENT;

        char prefix[PLASMA_PATH_RESOLVE_MAX];
        size_t prefix_n = 0;
        bool restart = false;

        while (*scan != '\0') {
            while (*scan == '/') ++scan;
            if (*scan == '\0') break;
            const char *segment = scan;
            while (*scan != '\0' && *scan != '/') ++scan;
            const size_t segment_n = (size_t)(scan - segment);
            const bool final_component = *scan == '\0';
            const size_t parent_n = prefix_n;

            if (prefix_n == 0) prefix[prefix_n++] = '/';
            else if (prefix[prefix_n - 1u] != '/') prefix[prefix_n++] = '/';
            if (prefix_n + segment_n + 1u > sizeof(prefix)) return -LINUX_ENAMETOOLONG;
            bytes_copy(prefix + prefix_n, segment, segment_n);
            prefix_n += segment_n;
            prefix[prefix_n] = '\0';

            struct rootfs_node node;
            if (!rootfs_lookup(prefix, &node)) return -LINUX_ENOENT;
            const bool symlink = (node.mode & 0170000u) == 0120000u;
            if (symlink && (!final_component || follow_final)) {
                if (++followed > PLASMA_SYMLINK_FOLLOW_MAX) return -LINUX_ELOOP;
                if (node.size == 0 || node.size >= PLASMA_PATH_RESOLVE_MAX)
                    return -LINUX_ENOENT;

                char target[PLASMA_PATH_RESOLVE_MAX];
                bytes_copy(target, node.data, node.size);
                target[node.size] = '\0';

                char combined[PLASMA_PATH_RESOLVE_MAX];
                size_t combined_n = 0;
                if (target[0] == '/') {
                    const size_t target_n = string_length(target);
                    if (target_n + 1u > sizeof(combined)) return -LINUX_ENAMETOOLONG;
                    bytes_copy(combined, target, target_n);
                    combined_n = target_n;
                } else {
                    if (parent_n == 0) {
                        combined[combined_n++] = '/';
                    } else {
                        if (parent_n + 1u > sizeof(combined)) return -LINUX_ENAMETOOLONG;
                        bytes_copy(combined, prefix, parent_n);
                        combined_n = parent_n;
                        if (combined_n == 0 || combined[combined_n - 1u] != '/')
                            combined[combined_n++] = '/';
                    }
                    const size_t target_n = string_length(target);
                    if (combined_n + target_n + 1u > sizeof(combined))
                        return -LINUX_ENAMETOOLONG;
                    bytes_copy(combined + combined_n, target, target_n);
                    combined_n += target_n;
                }

                const size_t remainder_n = string_length(scan);
                if (combined_n + remainder_n + 1u > sizeof(combined))
                    return -LINUX_ENAMETOOLONG;
                bytes_copy(combined + combined_n, scan, remainder_n + 1u);
                if (!plasma_resolve_path(combined, current, sizeof(current)))
                    return -LINUX_ENAMETOOLONG;
                restart = true;
                break;
            }

            if (!final_component && (node.mode & 0170000u) != S_IFDIR)
                return -LINUX_ENOTDIR;
            if (final_component) {
                *out = node;
                return 0;
            }
        }
        if (!restart) return -LINUX_ENOENT;
    }
}

static int64_t plasma_stat_resolved_path(const char *resolved,
                                         bool follow_final,
                                         uint64_t stat_address) {
    if (resolved == 0) return -LINUX_EFAULT;
    if (plasma_runtime_path(resolved)) {
        const int node = plasma_find_tmp(resolved);
        return node >= 0 ? plasma_tmp_stat(stat_address, node) : -LINUX_ENOENT;
    }
    if (string_equal(resolved, "/dev/tty") || string_equal(resolved, "/dev/null"))
        return fill_stat(stat_address, S_IFCHR | 0666u);
    if (string_equal(resolved, "/dev/fb0"))
        return plasma_fb_available() ? fill_stat(stat_address, S_IFCHR | 0666u) : -LINUX_ENOENT;

    struct rootfs_node node;
    const int64_t rc = plasma_rootfs_lookup_linux(resolved, follow_final, &node);
    return rc == 0 ? fill_rootfs_stat(stat_address, &node) : rc;
}

static int64_t plasma_fstat_current_fd(int fd, uint64_t stat_address) {
    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) return plasma_runtime_fstat(fd, stat_address);
    struct rootfs_open_file *root = rootfs_file_for_fd(fd);
    if (root != 0) return fill_rootfs_stat(stat_address, &root->node);
    if (fd_is_tty(fd) || fd == 4) return fill_stat(stat_address, S_IFCHR | 0666u);
#ifdef PLASMA_FB_FD
    if (fd == PLASMA_FB_FD && plasma_fb_available())
        return fill_stat(stat_address, S_IFCHR | 0666u);
#endif
    return -LINUX_EBADF;
}

static int64_t plasma_stat_at(int dirfd, uint64_t path_address,
                              uint64_t stat_address, uint32_t flags) {
    const uint32_t supported = PLASMA_AT_SYMLINK_NOFOLLOW |
                               PLASMA_AT_NO_AUTOMOUNT |
                               PLASMA_AT_EMPTY_PATH;
    if ((flags & ~supported) != 0) return -LINUX_EINVAL;

    char input[256];
    if (!copy_user_string(path_address, input, sizeof(input))) return -LINUX_EFAULT;
    if (input[0] == '\0') {
        if ((flags & PLASMA_AT_EMPTY_PATH) == 0) return -LINUX_ENOENT;
        return plasma_fstat_current_fd(dirfd, stat_address);
    }

    char resolved[PLASMA_PATH_RESOLVE_MAX];
    const int64_t rc = plasma_resolve_at_path(dirfd, input, resolved, sizeof(resolved));
    if (rc != 0) return rc;
    return plasma_stat_resolved_path(resolved,
        (flags & PLASMA_AT_SYMLINK_NOFOLLOW) == 0, stat_address);
}

static int64_t plasma_open_resolved_path(const char *resolved,
                                         uint32_t flags, uint32_t mode) {
    if (resolved == 0) return -LINUX_EFAULT;
    if (string_equal(resolved, "/dev/tty")) return 3;
    if (string_equal(resolved, "/dev/null")) return 4;
    if (string_equal(resolved, "/dev/fb0")) {
        if (!plasma_fb_available()) return -LINUX_ENODEV;
        return PLASMA_FB_FD;
    }
    if (plasma_runtime_path(resolved)) return plasma_open_runtime(resolved, flags, mode);

    struct rootfs_node node;
    const int64_t rc = plasma_rootfs_lookup_linux(resolved, true, &node);
    if (rc != 0) return rc;
    return rootfs_allocate_fd(&node);
}

static int64_t plasma_open_at(int dirfd, uint64_t path_address,
                              uint32_t flags, uint32_t mode) {
    char input[256];
    if (!copy_user_string(path_address, input, sizeof(input))) return -LINUX_EFAULT;
    char resolved[PLASMA_PATH_RESOLVE_MAX];
    const int64_t rc = plasma_resolve_at_path(dirfd, input, resolved, sizeof(resolved));
    if (rc != 0) return rc;
    return plasma_open_resolved_path(resolved, flags, mode);
}

static int64_t plasma_readlink_at(int dirfd, uint64_t path_address,
                                  uint64_t buffer_address, uint64_t capacity) {
    if (capacity == 0) return -LINUX_EINVAL;
    char input[256];
    if (!copy_user_string(path_address, input, sizeof(input))) return -LINUX_EFAULT;
    char resolved[PLASMA_PATH_RESOLVE_MAX];
    const int64_t rc = plasma_resolve_at_path(dirfd, input, resolved, sizeof(resolved));
    if (rc != 0) return rc;

    const char *synthetic = 0;
    if (string_equal(resolved, "/sys/class/graphics/fb0/device/subsystem"))
        synthetic = "../../../bus/platform";
    else if (string_equal(resolved, "/sys/class/graphics/fb0"))
        synthetic = "../../devices/platform/twilight-framebuffer.0/graphics/fb0";
    else if (string_equal(resolved, "/proc/self/exe"))
        synthetic = plasma_exec_path;

    if (synthetic != 0) {
        size_t n = string_length(synthetic);
        if (n > capacity) n = (size_t)capacity;
        return user_copy_out(buffer_address, synthetic, n) ? (int64_t)n : -LINUX_EFAULT;
    }

    struct rootfs_node node;
    const int64_t lookup = plasma_rootfs_lookup_linux(resolved, false, &node);
    if (lookup != 0) return lookup;
    if ((node.mode & 0170000u) != 0120000u) return -LINUX_EINVAL;
    size_t n = node.size;
    if (n > capacity) n = (size_t)capacity;
    return user_copy_out(buffer_address, node.data, n) ? (int64_t)n : -LINUX_EFAULT;
}

static bool plasma_mode_size_resolved(const char *resolved, bool follow_final,
                                      uint32_t *mode, uint64_t *size, uint64_t *ino) {
    if (resolved == 0 || mode == 0 || size == 0 || ino == 0) return false;
    if (plasma_runtime_path(resolved)) {
        const int index = plasma_find_tmp(resolved);
        if (index < 0) return false;
        *mode = plasma_tmp_nodes[index].mode;
        *size = plasma_tmp_nodes[index].size;
        *ino = 0x100000ull + (uint64_t)index;
        return true;
    }
    if (string_equal(resolved, "/dev/tty") || string_equal(resolved, "/dev/null")) {
        *mode = S_IFCHR | 0666u; *size = 0; *ino = 3; return true;
    }
    if (string_equal(resolved, "/dev/fb0") && plasma_fb_available()) {
        *mode = S_IFCHR | 0666u; *size = framebuffer_size(); *ino = 5; return true;
    }
    struct rootfs_node node;
    if (plasma_rootfs_lookup_linux(resolved, follow_final, &node) != 0) return false;
    *mode = node.mode;
    *size = node.size;
    *ino = plasma_rootfs_inode(&node);
    return true;
}

'''

    anchor = "static int64_t plasma_kde_statx(int dirfd,\n"
    text = rep(text, anchor, helpers + anchor)

    # statx has the same dirfd/relative-path and final-symlink rules as other
    # *at() calls.  AT_STATX_SYNC_* policy bits do not affect our in-memory CPIO.
    statx_pattern = re.compile(
        r"(?ms)^static int64_t plasma_kde_statx\(int dirfd,\n.*?^\}\n\n(?=static int64_t plasma_kde_statfs_out)"
    )
    statx_new = r'''static int64_t plasma_kde_statx(int dirfd,
                                uint64_t path_address,
                                uint32_t flags,
                                uint32_t mask,
                                uint64_t statx_address) {
    (void)mask;
    const uint32_t path_flags = PLASMA_AT_SYMLINK_NOFOLLOW |
                                PLASMA_AT_NO_AUTOMOUNT |
                                PLASMA_AT_EMPTY_PATH;
    /* STATX_FORCE_SYNC/STATX_DONT_SYNC (0x2000/0x4000) are accepted; the CPIO
     * and runtime VFS are memory-resident so both have identical behaviour. */
    if ((flags & ~(path_flags | 0x6000u)) != 0) return -LINUX_EINVAL;

    char input[256];
    if (!copy_user_string(path_address, input, sizeof(input))) return -LINUX_EFAULT;
    uint32_t mode = 0;
    uint64_t size = 0, ino = 0;

    if (input[0] == '\0') {
        if ((flags & PLASMA_AT_EMPTY_PATH) == 0) return -LINUX_ENOENT;
        if (!plasma_kde_mode_size_for_fd(dirfd, &mode, &size)) return -LINUX_EBADF;
        struct rootfs_open_file *root = rootfs_file_for_fd(dirfd);
        if (root != 0) ino = plasma_rootfs_inode(&root->node);
        else {
            struct plasma_runtime_fd *runtime = plasma_runtime_fd(dirfd);
            ino = runtime != 0 ? 0x100000ull + runtime->object : 0x200000ull + (uint32_t)dirfd;
        }
    } else {
        char resolved[PLASMA_PATH_RESOLVE_MAX];
        const int64_t rc = plasma_resolve_at_path(dirfd, input, resolved, sizeof(resolved));
        if (rc != 0) return rc;
        if (!plasma_mode_size_resolved(resolved,
                (flags & PLASMA_AT_SYMLINK_NOFOLLOW) == 0,
                &mode, &size, &ino)) return -LINUX_ENOENT;
    }
    return plasma_kde_fill_statx(statx_address, mode, size, ino);
}

'''
    text, statx_count = statx_pattern.subn(lambda _m: statx_new, text, count=1)
    if statx_count != 1:
        raise RuntimeError(f"expected exactly one plasma_kde_statx function, found {statx_count}")

    # Use consistent d_ino identity between getdents64 and stat/statx.  Qt is
    # allowed to compare the readdir inode with a subsequent stat result.
    text = rep(
        text,
        "                                                   index + 2u, (int64_t)cursor,\n",
        "                                                   plasma_rootfs_inode(&node), (int64_t)cursor,\n",
    )

    text = rep(
        text,
        "    case SYS_STAT:\n    case SYS_LSTAT: return stat_path(a1, a2);\n"
        "    case SYS_NEWFSTATAT: return stat_path(a2, a3);\n",
        "    case SYS_STAT: return plasma_stat_at(PLASMA_AT_FDCWD, a1, a2, 0);\n"
        "    case SYS_LSTAT: return plasma_stat_at(PLASMA_AT_FDCWD, a1, a2, PLASMA_AT_SYMLINK_NOFOLLOW);\n"
        "    case SYS_NEWFSTATAT: return plasma_stat_at((int)a1, a2, a3, (uint32_t)a4);\n",
    )

    text = rep(
        text,
        "    case SYS_OPEN: return sys_open_path(a1, (uint32_t)a2, (uint32_t)a3);\n"
        "    case SYS_OPENAT: return sys_open_path(a2, (uint32_t)a3, (uint32_t)a4);\n",
        "    case SYS_OPEN: return plasma_open_at(PLASMA_AT_FDCWD, a1, (uint32_t)a2, (uint32_t)a3);\n"
        "    case SYS_OPENAT: return plasma_open_at((int)a1, a2, (uint32_t)a3, (uint32_t)a4);\n",
    )

    # Replace the old proc/fbdev-only readlink cases.  readlinkat now respects
    # dirfd for relative paths and both expose real CPIO symlink targets.
    readlink_pattern = re.compile(
        r"(?ms)^    case SYS_READLINK: \{\n.*?^    \}\n(?=    case SYS_READLINKAT:)"
    )
    readlink_new = r'''    case SYS_READLINK:
        return plasma_readlink_at(PLASMA_AT_FDCWD, a1, a2, a3);
'''
    text, readlink_count = readlink_pattern.subn(lambda _m: readlink_new, text, count=1)
    if readlink_count != 1:
        raise RuntimeError(f"expected one SYS_READLINK case, found {readlink_count}")

    readlinkat_pattern = re.compile(
        r"(?ms)^    case SYS_READLINKAT: \{\n.*?^    \}\n(?=    case SYS_[A-Z0-9_]+:)"
    )
    readlinkat_new = r'''    case SYS_READLINKAT:
        return plasma_readlink_at((int)a1, a2, a3, a4);
'''
    text, readlinkat_count = readlinkat_pattern.subn(lambda _m: readlinkat_new, text, count=1)
    if readlinkat_count != 1:
        raise RuntimeError(f"expected one SYS_READLINKAT case, found {readlinkat_count}")

    # The old cwd-only helper remains used by no *at() call after this pass.
    # Keep it for compatibility with earlier transforms; all externally visible
    # open/stat/readlink operations now route through the Linux path resolver.

    path.write_text(text, encoding="utf-8")
    print(
        "Finalized Plasma VFS paths: dirfd-relative *at(), component symlinks, "
        "readlink, /proc/self/exe, and consistent dirent inode identity: " + str(path)
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
