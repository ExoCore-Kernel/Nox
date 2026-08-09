#!/usr/bin/env python3
"""Fix runtime-socket fcntl semantics and trace the first X11 setup writes.

Twilight's early AF_UNIX backend stored socket(2) creation flags directly in
plasma_runtime_fd.flags.  The generic runtime fcntl shim then returned those bits
from F_GETFL.  That is not Linux semantics: SOCK_CLOEXEC belongs to descriptor
flags (F_GETFD), while F_GETFL for a connected stream socket reports the file
status flags, normally O_RDWR plus O_NONBLOCK when enabled.  It also acknowledged
F_SETFL without changing O_NONBLOCK.

libX11/libxcb performs descriptor setup while opening DISPLAY.  Returning the
wrong fcntl state can make that setup fail without producing an unsupported-
syscall trace.  Correct the small fcntl subset and add concise TX diagnostics so
the next boot shows whether the X11 client actually attempts to send its setup
packet after connect().
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected X11 socket-fcntl fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # runtime_write is emitted before some later tracing helpers in the generated
    # unit.  An identical static prototype is harmless if another finalizer has
    # already introduced one elsewhere.
    write_decl_anchor = (
        "static void plasma_runtime_notify_pipe_write(int fd);\n"
        "static void plasma_runtime_notify_socket_data(uint16_t object);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n"
    )
    write_decl_new = (
        "static void serial_u64(uint64_t value);\n"
        "static void plasma_runtime_notify_pipe_write(int fd);\n"
        "static void plasma_runtime_notify_socket_data(uint16_t object);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n"
    )
    text = rep(text, write_decl_anchor, write_decl_new)

    # Trace every runtime-socket payload write.  write(2), writev(2), sendto(2)
    # and sendmsg(2) all funnel through this helper in the bring-up ABI.
    write_head_old = r'''static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (length == 0) return 0;
    if (!user_range(address, length, false)) return -LINUX_EFAULT;
'''
    write_head_new = r'''static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (entry->type == PLASMA_RT_SOCKET) {
        serial_write("[linux:socket] tx attempt fd=");
        serial_u64((uint64_t)fd);
        serial_write(" object=");
        serial_u64((uint64_t)entry->object);
        serial_write(" len=");
        serial_u64(length);
        serial_write("\n");
    }
    if (length == 0) return 0;
    if (!user_range(address, length, false)) {
        if (entry->type == PLASMA_RT_SOCKET)
            serial_write("[linux:socket] tx failed: user buffer EFAULT\n");
        return -LINUX_EFAULT;
    }
'''
    text = rep(text, write_head_old, write_head_new)

    socket_tail_old = r'''        if (done != 0) plasma_runtime_notify_socket_data((uint16_t)sock->peer);
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    socket_tail_new = r'''        if (done != 0) plasma_runtime_notify_socket_data((uint16_t)sock->peer);
        serial_write("[linux:socket] tx queued peer=");
        serial_u64((uint64_t)(sock->peer < 0 ? 0 : sock->peer));
        serial_write(" bytes=");
        serial_u64(done);
        serial_write("\n");
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    text = rep(text, socket_tail_old, socket_tail_new)

    # Correct the runtime fcntl subset. Linux F_GETFL does not return
    # O_CLOEXEC/SOCK_CLOEXEC, and sockets are read/write descriptors. F_SETFL
    # must actually update O_NONBLOCK because the cooperative read wrapper uses
    # that bit to distinguish blocking from nonblocking semantics.
    fcntl_old = r'''    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) {
        if (command == 1) return 0;
        if (command == 2 || command == 4) return 0;
        if (command == 3) return runtime->flags;
        if (command == 0 || command == 1030) {
            int copyfd = plasma_alloc_runtime_fd(runtime->type, runtime->object, runtime->flags);
            if (copyfd >= 0) plasma_runtime_fds[copyfd - PLASMA_RUNTIME_FD_FIRST].offset = runtime->offset;
            return copyfd;
        }
        return 0;
    }
'''
    fcntl_new = r'''    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) {
        if (runtime->type == PLASMA_RT_SOCKET) {
            serial_write("[linux:socket] fcntl fd=");
            serial_u64((uint64_t)fd);
            serial_write(" cmd=");
            serial_u64(command);
            serial_write(" arg=");
            serial_u64(argument);
            serial_write("\n");
        }

        if (command == 1) { /* F_GETFD */
            return (runtime->flags & PLASMA_SOCK_CLOEXEC) != 0 ? 1 : 0;
        }
        if (command == 2) { /* F_SETFD */
            if ((argument & 1u) != 0) runtime->flags |= PLASMA_SOCK_CLOEXEC;
            else runtime->flags &= ~PLASMA_SOCK_CLOEXEC;
            return 0;
        }
        if (command == 3) { /* F_GETFL */
            const uint32_t O_RDWR_VALUE = 2u;
            return (int64_t)(O_RDWR_VALUE | (runtime->flags & PLASMA_O_NONBLOCK));
        }
        if (command == 4) { /* F_SETFL */
            runtime->flags = (runtime->flags & ~PLASMA_O_NONBLOCK) |
                             ((uint32_t)argument & PLASMA_O_NONBLOCK);
            return 0;
        }
        if (command == 0 || command == 1030) { /* F_DUPFD / F_DUPFD_CLOEXEC */
            uint32_t copy_flags = runtime->flags;
            if (command == 1030) copy_flags |= PLASMA_SOCK_CLOEXEC;
            int copyfd = plasma_alloc_runtime_fd(runtime->type, runtime->object, copy_flags);
            if (copyfd >= 0)
                plasma_runtime_fds[copyfd - PLASMA_RUNTIME_FD_FIRST].offset = runtime->offset;
            return copyfd;
        }
        return 0;
    }
'''
    text = rep(text, fcntl_old, fcntl_new)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Linux-compatible runtime socket fcntl + X11 TX tracing: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
