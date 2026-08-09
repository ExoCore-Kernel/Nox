#!/usr/bin/env python3
"""Add the small writable-VFS syscalls Xorg uses to claim display :0.

Xorg creates a temporary file in /tmp, adjusts its mode, then link(2)s it to
/tmp/.X0-lock as an atomic claim.  Twilight's bring-up VFS stores file payloads
inline rather than as shared inode objects, so link() duplicates the runtime
node's metadata/data under the destination name.  Xorg immediately unlinks the
temporary source after a successful claim, making that equivalent for this
bring-up path while keeping the implementation simple.
"""
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
        "#define SYS_CHDIR           80ull\n",
        "#define SYS_CHDIR           80ull\n"
        "#define SYS_LINK            86ull\n"
        "#define SYS_FCHMOD          91ull\n",
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
    helpers = r'''static int64_t plasma_runtime_fchmod(int fd, uint32_t mode) {
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

    dispatch_anchor = "    case SYS_GETCWD: {\n"
    dispatch = r'''    case SYS_LINK:
        return plasma_runtime_link(a1, a2);
    case SYS_FCHMOD:
        return plasma_runtime_fchmod((int)a1, (uint32_t)a2);
'''
    text = rep(text, dispatch_anchor, dispatch + dispatch_anchor)

    # Bash/musl probes these while establishing credentials.  Twilight's
    # bring-up userspace runs as uid/gid 0, so Linux semantics are simply to
    # return the previous fsuid/fsgid, also 0.
    text = rep(
        text,
        "    case SYS_GETSID: return 1;\n",
        "    case SYS_GETSID: return 1;\n"
        "    case SYS_SETFSUID:\n"
        "    case SYS_SETFSGID: return 0;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Xorg /tmp lock link+fchmod ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
