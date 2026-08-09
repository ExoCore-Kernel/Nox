#!/usr/bin/env python3
"""Add cooperative blocking AF_UNIX reads for X11/Plasma bring-up.

The runtime socket backend originally returned EAGAIN whenever a connected
AF_UNIX stream had no queued bytes.  That is correct for O_NONBLOCK, but normal
X11 clients use blocking sockets: after connect() they send the setup request
and wait for Xorg's reply.  Returning EAGAIN before Xorg gets another scheduler
turn makes libxcb report that DISPLAY cannot be reached even though the X server
is alive.

Reuse the existing PLASMA_PROC_BLOCKED_IO scheduler path used by D-Bus pipes.
A blocking socket read saves the exact syscall-boundary context and yields.  A
write to its peer copies queued bytes directly into the sleeping reader's user
buffer, records the read return value, and makes that process runnable again.
readv already funnels through SYS_READ; recvmsg is wrapped here as well.

Do not put struct shell_image on the transition stack.  The Plasma page budget
makes it very large, so socket wakeups reuse plasma_scheduler_scratch_image just
like the proven pipe wake path.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected socket-I/O fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Keep pipe and socket wait identities separate while sharing BLOCKED_IO.
    text = rep(
        text,
        "    uint16_t blocked_pipe_object;\n"
        "    uint64_t blocked_io_address;\n",
        "    uint16_t blocked_pipe_object;\n"
        "    uint16_t blocked_socket_object;\n"
        "    uint64_t blocked_io_address;\n",
    )

    # The socket write helper is emitted before the process table but needs to
    # notify sleeping readers implemented later in the generated unit.
    text = rep(
        text,
        "static void plasma_runtime_notify_pipe_write(int fd);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n",
        "static void plasma_runtime_notify_pipe_write(int fd);\n"
        "static void plasma_runtime_notify_socket_data(uint16_t object);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n",
    )

    socket_write_old = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *sock = &plasma_sockets[entry->object];
        if (sock->peer < 0 || sock->peer >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[sock->peer].used)
            return -LINUX_EPIPE;
        struct plasma_socket_object *peer = &plasma_sockets[sock->peer];
        uint64_t done = 0;
        while (done < length) {
            size_t next = (peer->tail + 1u) % PLASMA_SOCKET_BUFFER;
            if (next == peer->head) break;
            uint8_t byte = 0;
            if (!user_copy_in(&byte, address + done, 1)) return -LINUX_EFAULT;
            peer->data[peer->tail] = byte;
            peer->tail = next;
            ++done;
        }
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    socket_write_new = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *sock = &plasma_sockets[entry->object];
        if (sock->peer < 0 || sock->peer >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[sock->peer].used)
            return -LINUX_EPIPE;
        struct plasma_socket_object *peer = &plasma_sockets[sock->peer];
        uint64_t done = 0;
        while (done < length) {
            size_t next = (peer->tail + 1u) % PLASMA_SOCKET_BUFFER;
            if (next == peer->head) break;
            uint8_t byte = 0;
            if (!user_copy_in(&byte, address + done, 1)) return -LINUX_EFAULT;
            peer->data[peer->tail] = byte;
            peer->tail = next;
            ++done;
        }
        if (done != 0) plasma_runtime_notify_socket_data((uint16_t)sock->peer);
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    text = rep(text, socket_write_old, socket_write_new)

    # recvmsg is defined before scheduler helpers, so forward-declare the
    # cooperative result wrapper and feed its first empty blocking read into the
    # same scheduler path as read/readv.
    text = rep(
        text,
        "static int64_t plasma_recvmsg(int fd, uint64_t msg_address) {\n",
        "static int64_t plasma_cooperative_socket_read_result(int fd, uint64_t address,\n"
        "                                                     uint64_t length, int64_t result);\n\n"
        "static int64_t plasma_recvmsg(int fd, uint64_t msg_address) {\n",
    )
    text = rep(
        text,
        "        int64_t rc = plasma_runtime_read(fd, iov.base, iov.len);\n"
        "        if (rc < 0) return total != 0 ? total : rc;\n",
        "        int64_t rc = plasma_runtime_read(fd, iov.base, iov.len);\n"
        "        rc = plasma_cooperative_socket_read_result(fd, iov.base, iov.len, rc);\n"
        "        if (rc < 0) return total != 0 ? total : rc;\n",
    )

    helper_anchor = "static int64_t plasma_schedule_after_syscall(int64_t result) {\n"
    helpers = r'''static int64_t plasma_cooperative_socket_read_result(int fd,
                                                       uint64_t address,
                                                       uint64_t length,
                                                       int64_t result) {
    if (result != -LINUX_EAGAIN) return result;

    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET ||
        entry->object >= PLASMA_SOCKET_OBJECTS)
        return result;
    if ((entry->flags & PLASMA_O_NONBLOCK) != 0) return result;

    struct plasma_socket_object *sock = &plasma_sockets[entry->object];
    if (!sock->used || sock->listening) return result;

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return result;

    plasma_save_active(current);
    /* UINT16_MAX can never be a pipe object and prevents the existing pipe EOF
     * wake code from mistaking a blocked socket with the same numeric object. */
    current->blocked_pipe_object = UINT16_MAX;
    current->blocked_socket_object = entry->object;
    current->blocked_io_address = address;
    current->blocked_io_length = length;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_IO;

    serial_write("[linux:socket] blocking read pid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" object=");
    serial_u64((uint64_t)entry->object);
    serial_write(" len=");
    serial_u64(length);
    serial_write("\n");
    return 0;
}

static void plasma_runtime_notify_socket_data(uint16_t object) {
    if (object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[object].used) return;
    struct plasma_socket_object *sock = &plasma_sockets[object];
    if (sock->head == sock->tail) return;

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *reader = &plasma_processes[i];
        if (reader->state != PLASMA_PROC_BLOCKED_IO ||
            reader->blocked_pipe_object != UINT16_MAX ||
            reader->blocked_socket_object != object)
            continue;

        bytes_copy(&plasma_scheduler_scratch_image, &image, sizeof(image));
        bytes_copy(&image, &reader->image, sizeof(image));

        uint64_t done = 0;
        bool fault = false;
        while (done < reader->blocked_io_length && sock->head != sock->tail) {
            const uint8_t byte = sock->data[sock->head];
            sock->head = (sock->head + 1u) % PLASMA_SOCKET_BUFFER;
            if (!user_copy_out(reader->blocked_io_address + done, &byte, 1)) {
                fault = true;
                break;
            }
            ++done;
        }

        bytes_copy(&image, &plasma_scheduler_scratch_image, sizeof(image));

        reader->pending_result = done != 0 ? (int64_t)done :
                                 (fault ? -LINUX_EFAULT : -LINUX_EAGAIN);
        reader->blocked_pipe_object = 0;
        reader->blocked_socket_object = 0;
        reader->blocked_io_address = 0;
        reader->blocked_io_length = 0;
        reader->state = PLASMA_PROC_RUNNABLE;

        serial_write("[linux:socket] woke reader pid=");
        serial_u64((uint64_t)reader->pid);
        serial_write(" object=");
        serial_u64((uint64_t)object);
        serial_write(" bytes=");
        serial_u64(done);
        serial_write("\n");
        return;
    }
}

'''
    text = rep(text, helper_anchor, helpers + helper_anchor)

    # readv delegates through SYS_READ, so changing the common runtime-read
    # wrapper covers both read(2) and readv(2).
    read_old = (
        "    case SYS_READ:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) {\n"
        "            const int64_t runtime_result = plasma_runtime_read((int)a1, a2, a3);\n"
        "            return plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "        }\n"
    )
    read_new = (
        "    case SYS_READ:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) {\n"
        "            const int64_t runtime_result = plasma_runtime_read((int)a1, a2, a3);\n"
        "            const int64_t pipe_result = plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (pipe_result != runtime_result) return pipe_result;\n"
        "            return plasma_cooperative_socket_read_result((int)a1, a2, a3, runtime_result);\n"
        "        }\n"
    )
    text = rep(text, read_old, read_new)

    # Add concise connection diagnostics.  This does not change connect()'s
    # matching rules; it makes a namespace/path failure distinguishable from a
    # later blocked-read failure on the next boot.
    text = rep(
        text,
        "    if (listener < 0) return -LINUX_ECONNREFUSED;\n"
        "    if (plasma_sockets[listener].pending >= 0) return -LINUX_EAGAIN;\n",
        "    if (listener < 0) {\n"
        "        serial_write(\"[linux:socket] connect refused: no matching listener\\n\");\n"
        "        return -LINUX_ECONNREFUSED;\n"
        "    }\n"
        "    if (plasma_sockets[listener].pending >= 0) {\n"
        "        serial_write(\"[linux:socket] connect would block: listener already pending\\n\");\n"
        "        return -LINUX_EAGAIN;\n"
        "    }\n",
    )
    text = rep(
        text,
        "    plasma_sockets[listener].pending = server;\n"
        "    return 0;\n",
        "    plasma_sockets[listener].pending = server;\n"
        "    serial_write(\"[linux:socket] connect queued listener=\");\n"
        "    serial_u64((uint64_t)listener);\n"
        "    serial_write(\" client=\");\n"
        "    serial_u64((uint64_t)entry->object);\n"
        "    serial_write(\" server=\");\n"
        "    serial_u64((uint64_t)server);\n"
        "    serial_write(\"\\n\");\n"
        "    return 0;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized cooperative AF_UNIX socket reads + X11 wakeups: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
