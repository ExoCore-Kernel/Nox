#!/usr/bin/env python3
"""Finish the small Linux ABI surface needed by dbus-daemon during Plasma boot.

The session bus currently reaches its real AF_UNIX socket setup, but then stops
because Twilight lacks chmod(2) and inotify.  For bring-up we do not need a full
filesystem notification subsystem: D-Bus only needs to create watches for its
service directories.  Supply an inert nonblocking inotify descriptor that can
be registered with epoll but never reports events; static service files in the
read-only Plasma CPIO do not change during this boot stage.

Also make XDG_RUNTIME_DIR private (0700) and accept chmod on runtime files and
bound AF_UNIX socket paths.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected D-Bus ABI fragment not found: {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "#define SYS_FCHMOD          91ull\n",
        "#define SYS_CHMOD           90ull\n#define SYS_FCHMOD          91ull\n",
    )
    text = rep(
        text,
        "#define SYS_OPENAT         257ull\n",
        "#define SYS_INOTIFY_INIT   253ull\n"
        "#define SYS_INOTIFY_ADD_WATCH 254ull\n"
        "#define SYS_INOTIFY_RM_WATCH 255ull\n"
        "#define SYS_OPENAT         257ull\n",
    )
    text = rep(
        text,
        "#define SYS_DUP3           292ull\n",
        "#define SYS_DUP3           292ull\n#define SYS_INOTIFY_INIT1  294ull\n",
    )

    # add-plasma-epoll owns type 6, so keep the inert inotify object distinct.
    text = rep(
        text,
        "#define PLASMA_RT_EPOLL  6u\n",
        "#define PLASMA_RT_EPOLL  6u\n#define PLASMA_RT_INOTIFY 7u\n",
    )

    # D-Bus rejects a world-writable XDG_RUNTIME_DIR.  Index 2 is
    # /tmp/runtime-root and index 5 is /run/user/0; the XKB finalizer only
    # appends another directory, so these indices remain stable.
    text = rep(
        text,
        "        plasma_tmp_nodes[i].mode = S_IFDIR | 0777u;\n",
        "        plasma_tmp_nodes[i].mode = S_IFDIR | ((i == 2u || i == 5u) ? 0700u : 0777u);\n",
    )

    # An inotify fd is intentionally eventless for this static CPIO bring-up.
    # If userspace ever reads it directly, match an empty nonblocking inotify
    # queue rather than reporting EBADF.
    text = rep(
        text,
        "    if (entry->type == PLASMA_RT_SOCKET) {\n",
        "    if (entry->type == PLASMA_RT_INOTIFY) return -LINUX_EAGAIN;\n"
        "    if (entry->type == PLASMA_RT_SOCKET) {\n",
        1,
    )

    helper_anchor = "static bool copy_user_string(uint64_t address, char *out, size_t capacity);\n\n"
    helpers = r'''static int plasma_inotify_next_watch = 1;

static int64_t plasma_runtime_chmod(uint64_t path_address, uint32_t mode) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;

    if (plasma_runtime_path(path)) {
        const int node_index = plasma_find_tmp(path);
        if (node_index >= 0) {
            struct plasma_tmp_node *node = &plasma_tmp_nodes[node_index];
            const uint32_t type = node->mode & 0170000u;
            node->mode = type | (mode & 07777u);
            return 0;
        }

        /* AF_UNIX bind paths live in the socket table rather than tmp_nodes.
         * chmod is still meaningful to D-Bus as a security/setup operation;
         * accept it for a socket path that is actually bound. */
        for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i) {
            if (plasma_sockets[i].used && plasma_sockets[i].path[0] != '\0' &&
                string_equal(plasma_sockets[i].path, path))
                return 0;
        }
    }
    return -LINUX_ENOENT;
}

static int64_t plasma_inotify_init(uint32_t flags) {
    const int fd = plasma_alloc_runtime_fd(PLASMA_RT_INOTIFY, 0, flags);
    if (fd >= 0) {
        serial_write("[linux:dbus] inert inotify fd=");
        serial_u64((uint64_t)fd);
        serial_write("\n");
    }
    return fd;
}

static int64_t plasma_inotify_add_watch(int fd, uint64_t path_address, uint32_t mask) {
    (void)mask;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_INOTIFY) return -LINUX_EBADF;

    char watch_path[256];
    if (!copy_user_string(path_address, watch_path, sizeof(watch_path))) return -LINUX_EFAULT;
    const int watch = plasma_inotify_next_watch++;
    if (plasma_inotify_next_watch <= 0) plasma_inotify_next_watch = 1;
    serial_write("[linux:dbus] inotify watch ");
    serial_write(watch_path);
    serial_write(" wd=");
    serial_u64((uint64_t)watch);
    serial_write("\n");
    return watch;
}

static int64_t plasma_inotify_rm_watch(int fd, int watch) {
    (void)watch;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    return (entry != 0 && entry->type == PLASMA_RT_INOTIFY) ? 0 : -LINUX_EBADF;
}

'''
    text = rep(text, helper_anchor, helper_anchor + helpers)

    text = rep(
        text,
        "    case SYS_FCHMOD:\n        return plasma_runtime_fchmod((int)a1, (uint32_t)a2);\n",
        "    case SYS_CHMOD:\n"
        "        return plasma_runtime_chmod(a1, (uint32_t)a2);\n"
        "    case SYS_FCHMOD:\n"
        "        return plasma_runtime_fchmod((int)a1, (uint32_t)a2);\n",
    )

    inotify_dispatch = r'''    case SYS_INOTIFY_INIT:
        return plasma_inotify_init(0);
    case SYS_INOTIFY_INIT1:
        return plasma_inotify_init((uint32_t)a1);
    case SYS_INOTIFY_ADD_WATCH:
        return plasma_inotify_add_watch((int)a1, a2, (uint32_t)a3);
    case SYS_INOTIFY_RM_WATCH:
        return plasma_inotify_rm_watch((int)a1, (int)a2);
'''
    text = rep(
        text,
        "    case SYS_EPOLL_CREATE1: return plasma_epoll_create1((uint32_t)a1);\n",
        inotify_dispatch + "    case SYS_EPOLL_CREATE1: return plasma_epoll_create1((uint32_t)a1);\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized D-Bus chmod + private runtime dir + inert inotify ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
