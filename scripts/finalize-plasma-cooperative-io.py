#!/usr/bin/env python3
"""Finalize cooperative poll and blocking pipe semantics for Plasma bring-up.

The early runtime originally returned EAGAIN whenever a pipe read found no data.
That is correct only for O_NONBLOCK.  dbus-run-session uses a normal blocking
pipe to receive the private bus address from the dbus-daemon child, so returning
EAGAIN races the child and aborts the Plasma session before startplasma-x11 can
run.

This finalizer keeps the scheduler cooperative: an empty blocking pipe read saves
the caller's syscall-boundary context and marks that process BLOCKED_IO.  A later
write to the same shared pipe copies bytes directly into the blocked reader's
userspace buffer, records the read() return value, and makes it runnable again.
The existing scheduler then resumes the reader as though the original read(2)
had just returned.  Nonblocking pipes still receive EAGAIN.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected cooperative I/O fragment not found: {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Existing poll(2) compatibility: a wait involving runtime pipe/socket FDs
    # must return to the syscall boundary so the cooperative scheduler can run
    # another process instead of sleeping the whole kernel on the serial TTY.
    text = rep(
        text,
        "        bool wants_input = false;\n",
        "        bool wants_input = false;\n        bool runtime_wait = false;\n",
    )
    text = rep(
        text,
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
        "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
        "                runtime_wait = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
    )
    text = rep(
        text,
        "        if (ready != 0 || timeout == 0 || !wants_input) return ready;\n"
        "        tty_wait_for_input();\n",
        "        if (ready != 0 || timeout == 0 || (!wants_input && !runtime_wait)) return ready;\n"
        "        if (runtime_wait) return ready; /* syscall-boundary scheduler yield */\n"
        "        tty_wait_for_input();\n",
    )

    # Add a distinct blocked-I/O state. wait4 already proves the scheduler can
    # suspend a process at one syscall boundary and later resume it with a saved
    # pending return value; pipe reads use the same mechanism.
    text = rep(
        text,
        "    PLASMA_PROC_RUNNING,\n"
        "    PLASMA_PROC_BLOCKED_WAIT,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
        "    PLASMA_PROC_RUNNING,\n"
        "    PLASMA_PROC_BLOCKED_WAIT,\n"
        "    PLASMA_PROC_BLOCKED_IO,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
    )
    text = rep(
        text,
        "    uint64_t wait_rusage_address;\n"
        "    int64_t pending_result;\n",
        "    uint64_t wait_rusage_address;\n"
        "    uint16_t blocked_pipe_object;\n"
        "    uint64_t blocked_io_address;\n"
        "    uint64_t blocked_io_length;\n"
        "    int64_t pending_result;\n",
    )

    # Every runtime write path (write, writev, sendmsg) eventually funnels
    # through plasma_runtime_write(), so notify blocked readers there rather than
    # only in the SYS_WRITE switch case.
    text = rep(
        text,
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n",
        "static void plasma_runtime_notify_pipe_write(int fd);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n",
    )

    pipe_write_old = r'''    if (entry->type == PLASMA_RT_PIPE_W) {
        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];
        uint64_t done = 0;
        while (done < length) {
            size_t next = (pipe->tail + 1u) % PLASMA_PIPE_BUFFER;
            if (next == pipe->head) break;
            uint8_t byte = 0;
            if (!user_copy_in(&byte, address + done, 1)) return -LINUX_EFAULT;
            pipe->data[pipe->tail] = byte;
            pipe->tail = next;
            ++done;
        }
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    pipe_write_new = r'''    if (entry->type == PLASMA_RT_PIPE_W) {
        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];
        uint64_t done = 0;
        while (done < length) {
            size_t next = (pipe->tail + 1u) % PLASMA_PIPE_BUFFER;
            if (next == pipe->head) break;
            uint8_t byte = 0;
            if (!user_copy_in(&byte, address + done, 1)) return -LINUX_EFAULT;
            pipe->data[pipe->tail] = byte;
            pipe->tail = next;
            ++done;
        }
        if (done != 0) plasma_runtime_notify_pipe_write(fd);
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    text = rep(text, pipe_write_old, pipe_write_new)

    helper_anchor = "static int64_t plasma_schedule_after_syscall(int64_t result) {\n"
    helpers = r'''static int64_t plasma_cooperative_pipe_read_result(int fd,
                                                     uint64_t address,
                                                     uint64_t length,
                                                     int64_t result) {
    if (result != -LINUX_EAGAIN) return result;

    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_PIPE_R) return result;
    if ((entry->flags & PLASMA_O_NONBLOCK) != 0) return result;

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return result;

    /* Save the exact userspace return context before changing state. The normal
     * post-syscall scheduler deliberately skips saving non-RUNNING processes. */
    plasma_save_active(current);
    current->blocked_pipe_object = entry->object;
    current->blocked_io_address = address;
    current->blocked_io_length = length;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_IO;

    serial_write("[linux:ipc] blocking pipe read pid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" pipe=");
    serial_u64((uint64_t)entry->object);
    serial_write(" len=");
    serial_u64(length);
    serial_write("\n");
    return 0;
}

static void plasma_runtime_notify_pipe_write(int fd) {
    struct plasma_runtime_fd *writer = plasma_runtime_fd(fd);
    if (writer == 0 || writer->type != PLASMA_RT_PIPE_W ||
        writer->object >= PLASMA_PIPE_OBJECTS)
        return;

    struct plasma_pipe_object *pipe = &plasma_pipes[writer->object];
    if (pipe->head == pipe->tail) return;

    /* Linux pipes wake readers when data becomes available. Wake one waiter per
     * write; any remaining bytes stay queued and a later read/writer wake can
     * service another waiter. */
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *reader = &plasma_processes[i];
        if (reader->state != PLASMA_PROC_BLOCKED_IO ||
            reader->blocked_pipe_object != writer->object)
            continue;

        struct shell_image saved_image;
        bytes_copy(&saved_image, &image, sizeof(image));
        bytes_copy(&image, &reader->image, sizeof(image));

        uint64_t done = 0;
        bool fault = false;
        while (done < reader->blocked_io_length && pipe->head != pipe->tail) {
            const uint8_t byte = pipe->data[pipe->head];
            pipe->head = (pipe->head + 1u) % PLASMA_PIPE_BUFFER;
            if (!user_copy_out(reader->blocked_io_address + done, &byte, 1)) {
                fault = true;
                break;
            }
            ++done;
        }

        bytes_copy(&image, &saved_image, sizeof(image));

        reader->pending_result = done != 0 ? (int64_t)done :
                                 (fault ? -LINUX_EFAULT : -LINUX_EAGAIN);
        reader->blocked_pipe_object = 0;
        reader->blocked_io_address = 0;
        reader->blocked_io_length = 0;
        reader->state = PLASMA_PROC_RUNNABLE;

        serial_write("[linux:ipc] woke pipe reader pid=");
        serial_u64((uint64_t)reader->pid);
        serial_write(" bytes=");
        serial_u64(done);
        serial_write("\n");
        return;
    }
}

'''
    text = rep(text, helper_anchor, helpers + helper_anchor)

    # Convert an empty blocking runtime-pipe read from EAGAIN into a scheduler
    # block. This also covers readv because the XKB readv shim delegates each
    # vector through shell_dispatch(SYS_READ, ...).
    read_dispatch_old = (
        "    case SYS_READ:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_read((int)a1, a2, a3);\n"
        "        if ((int)a1 == 4) return 0; /* /dev/null */\n"
    )
    read_dispatch_new = (
        "    case SYS_READ:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) {\n"
        "            const int64_t runtime_result = plasma_runtime_read((int)a1, a2, a3);\n"
        "            return plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "        }\n"
        "        if ((int)a1 == 4) return 0; /* /dev/null */\n"
    )
    text = rep(text, read_dispatch_old, read_dispatch_new)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized cooperative poll + blocking pipe reads: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
