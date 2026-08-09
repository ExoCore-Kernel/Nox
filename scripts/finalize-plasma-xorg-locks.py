#!/usr/bin/env python3
"""Add the small writable-VFS, credential, and helper syscalls Xorg needs."""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected Xorg-lock source fragment not found: {old[:160]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "#define SYS_WRITEV          20ull\n",
        "#define SYS_READV           19ull\n#define SYS_WRITEV          20ull\n",
    )
    text = rep(
        text,
        "#define SYS_CHDIR           80ull\n",
        "#define SYS_CHDIR           80ull\n"
        "#define SYS_LINK            86ull\n"
        "#define SYS_FCHMOD          91ull\n",
    )
    text = rep(
        text,
        "#define SYS_GETGID         104ull\n",
        "#define SYS_GETGID         104ull\n"
        "#define SYS_SETUID         105ull\n"
        "#define SYS_SETGID         106ull\n",
    )
    text = rep(
        text,
        "#define SYS_SETSID         112ull\n#define SYS_GETGROUPS      115ull\n#define SYS_GETRESUID      118ull\n#define SYS_GETRESGID      120ull\n",
        "#define SYS_SETSID         112ull\n"
        "#define SYS_SETREUID       113ull\n"
        "#define SYS_SETREGID       114ull\n"
        "#define SYS_GETGROUPS      115ull\n"
        "#define SYS_SETGROUPS      116ull\n"
        "#define SYS_SETRESUID      117ull\n"
        "#define SYS_GETRESUID      118ull\n"
        "#define SYS_SETRESGID      119ull\n"
        "#define SYS_GETRESGID      120ull\n",
    )
    text = rep(
        text,
        "#define SYS_GETPGID        121ull\n",
        "#define SYS_GETPGID        121ull\n"
        "#define SYS_SETFSUID       122ull\n"
        "#define SYS_SETFSGID       123ull\n",
    )
    text = rep(
        text,
        "#define LINUX_EEXIST     17\n",
        "#define LINUX_EEXIST     17\n#define LINUX_EXDEV      18\n",
    )

    helper_anchor = "static int64_t plasma_runtime_lseek(int fd, int64_t offset, int whence) {\n"
    helpers = r'''static bool copy_user_string(uint64_t address, char *out, size_t capacity);

static int64_t plasma_runtime_fchmod(int fd, uint32_t mode) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (entry->type != PLASMA_RT_FILE && entry->type != PLASMA_RT_DIR)
        return -LINUX_EBADF;
    if (entry->object >= PLASMA_TMP_NODES || !plasma_tmp_nodes[entry->object].used)
        return -LINUX_EBADF;

    struct plasma_tmp_node *node = &plasma_tmp_nodes[entry->object];
    const uint32_t type = node->mode & 0170000u;
    node->mode = type | (mode & 07777u);
    return 0;
}

static int64_t plasma_runtime_link(uint64_t old_address, uint64_t new_address) {
    char old_path[256], new_path[256];
    if (!copy_user_string(old_address, old_path, sizeof(old_path)) ||
        !copy_user_string(new_address, new_path, sizeof(new_path)))
        return -LINUX_EFAULT;

    if (!plasma_runtime_path(old_path) || !plasma_runtime_path(new_path))
        return -LINUX_EXDEV;

    const int source_index = plasma_find_tmp(old_path);
    if (source_index < 0) return -LINUX_ENOENT;
    if (plasma_find_tmp(new_path) >= 0) return -LINUX_EEXIST;

    struct plasma_tmp_node *source = &plasma_tmp_nodes[source_index];
    if (source->directory) return -LINUX_EPERM;

    const int destination_index = plasma_create_tmp(new_path, false, source->mode & 07777u);
    if (destination_index < 0) return destination_index;

    struct plasma_tmp_node *destination = &plasma_tmp_nodes[destination_index];
    destination->mode = source->mode;
    destination->size = source->size;
    if (source->size != 0)
        bytes_copy(destination->data, source->data, source->size);
    return 0;
}

'''
    text = rep(text, helper_anchor, helpers + helper_anchor)

    # xkbcomp uses readv(2) on stdin. Feed every iovec through the existing
    # read path so pipes/runtime FDs keep exactly the same semantics as read(2).
    read_anchor = "    case SYS_WRITE:\n"
    readv_dispatch = r'''    case SYS_READV: {
        if (a3 > 64 || !user_range(a2, a3 * sizeof(struct linux_iovec), false))
            return -LINUX_EFAULT;
        int64_t total = 0;
        for (uint64_t i = 0; i < a3; ++i) {
            struct linux_iovec iov;
            if (!user_copy_in(&iov, a2 + i * sizeof(iov), sizeof(iov)))
                return total != 0 ? total : -LINUX_EFAULT;
            if (iov.len == 0) continue;
            const int64_t rc = shell_dispatch(SYS_READ, a1, iov.base, iov.len, 0, 0, 0);
            if (rc < 0) return total != 0 ? total : rc;
            total += rc;
            if ((uint64_t)rc < iov.len) break;
        }
        return total;
    }
'''
    text = rep(text, read_anchor, readv_dispatch + read_anchor)

    dispatch_anchor = "    case SYS_GETCWD: {\n"
    dispatch = r'''    case SYS_LINK:
        return plasma_runtime_link(a1, a2);
    case SYS_FCHMOD:
        return plasma_runtime_fchmod((int)a1, (uint32_t)a2);
'''
    text = rep(text, dispatch_anchor, dispatch + dispatch_anchor)

    credential_anchor = (
        "    case SYS_GETUID:\n"
        "    case SYS_GETGID:\n"
        "    case SYS_GETEUID:\n"
        "    case SYS_GETEGID: return 0;\n"
    )
    credential_dispatch = credential_anchor + (
        "    case SYS_SETUID:\n"
        "    case SYS_SETGID:\n"
        "    case SYS_SETREUID:\n"
        "    case SYS_SETREGID:\n"
        "    case SYS_SETGROUPS:\n"
        "    case SYS_SETRESUID:\n"
        "    case SYS_SETRESGID: return 0;\n"
    )
    text = rep(text, credential_anchor, credential_dispatch)

    text = rep(
        text,
        "    case SYS_GETSID: return 1;\n",
        "    case SYS_GETSID: return 1;\n"
        "    case SYS_SETFSUID:\n"
        "    case SYS_SETFSGID: return 0;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Xorg /tmp lock + root credential + readv ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
