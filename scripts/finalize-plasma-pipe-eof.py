#!/usr/bin/env python3
"""Finish Linux pipe EOF semantics for the cooperative Plasma runtime.

Blocking pipe reads were added for dbus-run-session, but an empty pipe was still
always treated as temporarily empty.  On Linux, once the final write descriptor
for a pipe is closed, readers must observe EOF (read returns 0).  D-Bus writes
its session-bus address to a startup pipe, closes that write side, and keeps the
daemon alive; without EOF the parent consumes the address and then blocks
forever waiting for a byte that can never arrive.

This finalizer derives writer liveness from every live process' inherited runtime
FD tables, wakes blocked readers when the final writer closes or exits, and makes
poll/epoll report an EOF pipe as readable so userspace can consume the zero-byte
read in the normal Linux way.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected pipe EOF fragment not found: {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # The runtime read/close/poll helpers are emitted before the process table,
    # while the implementation below needs the process table to inspect all
    # inherited descriptor copies.  Forward-declare the two cross-layer helpers.
    read_anchor = "static int64_t plasma_runtime_read(int fd, uint64_t address, uint64_t length) {\n"
    declarations = (
        "static bool plasma_pipe_has_live_writer(uint16_t object);\n"
        "static void plasma_runtime_notify_pipe_eof(uint16_t object);\n\n"
    )
    text = rep(text, read_anchor, declarations + read_anchor)

    # Empty pipe + no write descriptors is EOF, not EAGAIN.  The cooperative
    # read wrapper therefore returns 0 immediately instead of blocking.
    pipe_read_old = r'''        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
    if (entry->type == PLASMA_RT_INOTIFY) return -LINUX_EAGAIN;
'''
    pipe_read_new = r'''        return done != 0 ? (int64_t)done :
               (plasma_pipe_has_live_writer(entry->object) ? -LINUX_EAGAIN : 0);
    }
    if (entry->type == PLASMA_RT_INOTIFY) return -LINUX_EAGAIN;
'''
    text = rep(text, pipe_read_old, pipe_read_new)

    close_old = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    bytes_zero(entry, sizeof(*entry));
    return 0;
}
'''
    close_new = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const bool was_pipe_writer = entry->type == PLASMA_RT_PIPE_W &&
                                 entry->object < PLASMA_PIPE_OBJECTS;
    const uint16_t pipe_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    return 0;
}
'''
    text = rep(text, close_old, close_new)

    # Linux poll/epoll make the read end observable when the writer side is gone;
    # the subsequent read returns 0.  Treat that state as POLLIN for our small
    # level-triggered bring-up implementation.
    poll_old = r'''        else if (entry->type == PLASMA_RT_PIPE_R) {
            struct plasma_pipe_object *p = &plasma_pipes[entry->object];
            if (p->head != p->tail) *revents |= POLLIN;
        } else if (entry->type == PLASMA_RT_SOCKET) {
'''
    poll_new = r'''        else if (entry->type == PLASMA_RT_PIPE_R) {
            struct plasma_pipe_object *p = &plasma_pipes[entry->object];
            if (p->head != p->tail || !plasma_pipe_has_live_writer(entry->object))
                *revents |= POLLIN;
        } else if (entry->type == PLASMA_RT_SOCKET) {
'''
    text = rep(text, poll_old, poll_new)

    # Helpers are inserted after the process scheduler structures/functions have
    # been emitted, so they can inspect each process' high and low runtime FD
    # tables.  For the currently running process the global tables are newer than
    # the saved snapshot until the post-syscall scheduler saves it.
    helper_anchor = "static int64_t plasma_cooperative_pipe_read_result(int fd,\n"
    helpers = r'''static bool plasma_fd_table_has_pipe_writer(const struct plasma_runtime_fd *table,
                                            size_t count,
                                            uint16_t object) {
    if (table == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (table[i].used && table[i].type == PLASMA_RT_PIPE_W &&
            table[i].object == object)
            return true;
    }
    return false;
}

static bool plasma_pipe_has_live_writer(uint16_t object) {
    if (object >= PLASMA_PIPE_OBJECTS) return false;
    struct plasma_process *current = plasma_current_process();

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;

        if (process == current) {
            if (plasma_fd_table_has_pipe_writer(plasma_runtime_fds,
                                                PLASMA_RUNTIME_FD_COUNT, object) ||
                plasma_fd_table_has_pipe_writer(plasma_low_runtime_fds, 10u, object))
                return true;
        } else {
            if (plasma_fd_table_has_pipe_writer(process->runtime_fds,
                                                PLASMA_RUNTIME_FD_COUNT, object) ||
                plasma_fd_table_has_pipe_writer(process->low_runtime_fds, 10u, object))
                return true;
        }
    }
    return false;
}

static void plasma_runtime_notify_pipe_eof(uint16_t object) {
    if (object >= PLASMA_PIPE_OBJECTS) return;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *reader = &plasma_processes[i];
        if (reader->state != PLASMA_PROC_BLOCKED_IO ||
            reader->blocked_pipe_object != object)
            continue;

        reader->pending_result = 0; /* Linux read(2) EOF. */
        reader->blocked_pipe_object = 0;
        reader->blocked_io_address = 0;
        reader->blocked_io_length = 0;
        reader->state = PLASMA_PROC_RUNNABLE;

        serial_write("[linux:ipc] woke pipe reader pid=");
        serial_u64((uint64_t)reader->pid);
        serial_write(" EOF pipe=");
        serial_u64((uint64_t)object);
        serial_write("\n");
    }
}

'''
    text = rep(text, helper_anchor, helpers + helper_anchor)

    # A process can terminate without explicitly closing every inherited FD.
    # Once it becomes a zombie its write descriptors no longer count as live;
    # notify any pipe for which that transition removed the final writer.
    exit_old = r'''    process->exit_status = status & 0xff;
    process->state = PLASMA_PROC_ZOMBIE;
    plasma_wake_waiters_for(process);
'''
    exit_new = r'''    bool exiting_pipe_writers[PLASMA_PIPE_OBJECTS];
    bytes_zero(exiting_pipe_writers, sizeof(exiting_pipe_writers));
    for (size_t i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        if (plasma_runtime_fds[i].used && plasma_runtime_fds[i].type == PLASMA_RT_PIPE_W &&
            plasma_runtime_fds[i].object < PLASMA_PIPE_OBJECTS)
            exiting_pipe_writers[plasma_runtime_fds[i].object] = true;
    }
    for (size_t i = 0; i < 10u; ++i) {
        if (plasma_low_runtime_fds[i].used && plasma_low_runtime_fds[i].type == PLASMA_RT_PIPE_W &&
            plasma_low_runtime_fds[i].object < PLASMA_PIPE_OBJECTS)
            exiting_pipe_writers[plasma_low_runtime_fds[i].object] = true;
    }

    process->exit_status = status & 0xff;
    process->state = PLASMA_PROC_ZOMBIE;
    for (uint16_t object = 0; object < PLASMA_PIPE_OBJECTS; ++object)
        if (exiting_pipe_writers[object] && !plasma_pipe_has_live_writer(object))
            plasma_runtime_notify_pipe_eof(object);
    plasma_wake_waiters_for(process);
'''
    text = rep(text, exit_old, exit_new)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Linux pipe EOF + last-writer wake semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
