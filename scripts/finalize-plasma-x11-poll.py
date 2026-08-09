#!/usr/bin/env python3
"""Make runtime-socket poll/ppoll waits cooperative instead of fake timeouts.

The early Plasma scheduler originally handled runtime poll() by returning to the
syscall boundary with a result of 0 whenever no fd was ready.  That does let a
second process run, but userspace sees an actual poll timeout.  libX11/libxcb
uses a nonblocking AF_UNIX socket while XOpenDisplay performs the setup
handshake: it sends the 12-byte client hello and then polls for Xorg's reply.
Returning 0 before Xorg gets its turn makes XOpenDisplay abort even though the
server subsequently queues a valid setup reply.

For the current bring-up ABI, block the common single-runtime-socket POLLIN wait
cooperatively.  Socket data wakes the poller, writes POLLIN into its userspace
pollfd, returns 1 from the original poll syscall, and resumes the process.  More
general multi-fd timeout accounting can be added with the full scheduler later.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected X11 poll fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # A distinct state keeps a sleeping poll separate from blocked read() and
    # wait4().  The scheduler already skips all non-RUNNABLE states.
    text = rep(
        text,
        "    PLASMA_PROC_BLOCKED_IO,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
        "    PLASMA_PROC_BLOCKED_IO,\n"
        "    PLASMA_PROC_BLOCKED_POLL,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
    )

    # Only a single AF_UNIX fd is needed for the XOpenDisplay handshake. Store
    # the userspace pollfd address so the wake path can set revents before the
    # original syscall returns.
    text = rep(
        text,
        "    uint16_t blocked_socket_object;\n"
        "    uint64_t blocked_io_address;\n",
        "    uint16_t blocked_socket_object;\n"
        "    uint16_t blocked_poll_socket_object;\n"
        "    uint64_t blocked_poll_address;\n"
        "    uint64_t blocked_io_address;\n",
    )

    # sys_poll is emitted before the process table, so forward-declare the
    # cooperative blocker there and replace the old fake-timeout yield.
    poll_decl = (
        "static int64_t plasma_cooperative_runtime_poll_wait(uint64_t fds_address,\n"
        "                                                    uint64_t count,\n"
        "                                                    int64_t timeout,\n"
        "                                                    int ready);\n\n"
    )
    text = rep(
        text,
        "static int64_t sys_poll(uint64_t fds_address, uint64_t count, int64_t timeout) {\n",
        poll_decl + "static int64_t sys_poll(uint64_t fds_address, uint64_t count, int64_t timeout) {\n",
    )
    text = rep(
        text,
        "        if (runtime_wait) return ready; /* syscall-boundary scheduler yield */\n",
        "        if (runtime_wait)\n"
        "            return plasma_cooperative_runtime_poll_wait(fds_address, count, timeout, ready);\n",
    )

    # Insert the blocker after the process/runtime structures are available.
    helper_anchor = "static int64_t plasma_schedule_after_syscall(int64_t result) {\n"
    blocker = r'''static int64_t plasma_cooperative_runtime_poll_wait(uint64_t fds_address,
                                                    uint64_t count,
                                                    int64_t timeout,
                                                    int ready) {
    if (ready != 0 || timeout == 0) return ready;

    /* The X11 setup path polls exactly one connected runtime socket for input.
     * Preserve the old bring-up behaviour for other shapes until the general
     * poll waiter grows a full fd-set snapshot and timeout clock. */
    if (count != 1u) return ready;

    struct linux_pollfd pfd;
    if (!user_copy_in(&pfd, fds_address, sizeof(pfd))) return -LINUX_EFAULT;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(pfd.fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET ||
        entry->object >= PLASMA_SOCKET_OBJECTS ||
        (pfd.events & POLLIN) == 0)
        return ready;

    struct plasma_socket_object *sock = &plasma_sockets[entry->object];
    if (sock->head != sock->tail) {
        pfd.revents |= POLLIN;
        if (!user_copy_out(fds_address, &pfd, sizeof(pfd))) return -LINUX_EFAULT;
        return 1;
    }

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return ready;

    plasma_save_active(current);
    current->blocked_poll_socket_object = entry->object;
    current->blocked_poll_address = fds_address;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_POLL;

    serial_write("[linux:socket] blocking poll pid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" object=");
    serial_u64((uint64_t)entry->object);
    serial_write(" timeout=");
    serial_u64(timeout < 0 ? 0ull : (uint64_t)timeout);
    serial_write("\n");
    return 0;
}

'''
    text = rep(text, helper_anchor, blocker + helper_anchor)

    # socket-io already owns the single data-arrival notification funnel.  Wake
    # any poll waiter before servicing a direct blocked read on the same object.
    wake_anchor = r'''static void plasma_runtime_notify_socket_data(uint16_t object) {
    if (object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[object].used) return;
    struct plasma_socket_object *sock = &plasma_sockets[object];
    if (sock->head == sock->tail) return;

'''
    wake_new = wake_anchor + r'''    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *poller = &plasma_processes[i];
        if (poller->state != PLASMA_PROC_BLOCKED_POLL ||
            poller->blocked_poll_socket_object != object ||
            poller->blocked_poll_address == 0)
            continue;

        bytes_copy(&plasma_scheduler_scratch_image, &image, sizeof(image));
        bytes_copy(&image, &poller->image, sizeof(image));

        struct linux_pollfd pfd;
        bool ok = user_copy_in(&pfd, poller->blocked_poll_address, sizeof(pfd));
        if (ok) {
            pfd.revents = 0;
            if ((pfd.events & POLLIN) != 0) pfd.revents |= POLLIN;
            ok = user_copy_out(poller->blocked_poll_address, &pfd, sizeof(pfd));
        }

        bytes_copy(&image, &plasma_scheduler_scratch_image, sizeof(image));

        poller->pending_result = ok ? 1 : -LINUX_EFAULT;
        poller->blocked_poll_socket_object = 0;
        poller->blocked_poll_address = 0;
        poller->state = PLASMA_PROC_RUNNABLE;

        serial_write("[linux:socket] woke poll pid=");
        serial_u64((uint64_t)poller->pid);
        serial_write(" object=");
        serial_u64((uint64_t)object);
        serial_write(" revents=POLLIN\n");
        break;
    }

'''
    text = rep(text, wake_anchor, wake_new)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized cooperative runtime-socket poll/ppoll waits for X11: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
