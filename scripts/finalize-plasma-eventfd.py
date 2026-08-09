#!/usr/bin/env python3
"""Implement Linux eventfd/eventfd2 for the Plasma runtime.

GLib's GWakeup uses eventfd2 on Linux.  Twilight previously returned ENOSYS,
forcing GLib onto its pipe fallback and exposing the tiny/leaky bring-up pipe
pool.  This finalizer implements eventfd as a real kernel-backed counter object,
not as a success stub.

Implemented semantics for the current cooperative runtime:
* eventfd/eventfd2 creation with EFD_SEMAPHORE, EFD_NONBLOCK and EFD_CLOEXEC;
* 64-bit counter read/write rules, including UINT64_MAX rejection and overflow;
* blocking reads and blocking overflow writes using the scheduler's BLOCKED_IO
  state, with wakeups when the counter becomes readable/writable;
* poll/epoll readability and writability;
* shared object identity across dup/fork/CLONE_FILES descriptor copies;
* last-reference reclamation and eventfd-specific close-on-exec handling.

The wider runtime fd layer still has known Linux-model limitations (for example,
file-status flags are stored in descriptor snapshots rather than a separate open
file-description object).  This file does not claim to solve those unrelated fd
model issues.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected eventfd fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # x86_64 Linux syscall numbers.
    text = rep(text,
               "#define SYS_EPOLL_PWAIT    281ull\n",
               "#define SYS_EPOLL_PWAIT    281ull\n#define SYS_EVENTFD        284ull\n")
    text = rep(text,
               "#define SYS_EPOLL_CREATE1  291ull\n",
               "#define SYS_EVENTFD2       290ull\n#define SYS_EPOLL_CREATE1  291ull\n")

    # Runtime type 6=epoll and 7=inotify in the current pipeline.
    text = rep(text,
               "#define PLASMA_RT_INOTIFY 7u\n",
               "#define PLASMA_RT_INOTIFY 7u\n#define PLASMA_RT_EVENTFD 8u\n")

    pipe_struct = r'''struct plasma_pipe_object {
    bool used;
    uint8_t data[PLASMA_PIPE_BUFFER];
    size_t head;
    size_t tail;
};
'''
    eventfd_struct = pipe_struct + r'''
#define PLASMA_EVENTFD_OBJECTS 64
#define PLASMA_EFD_SEMAPHORE 00000001u
#define PLASMA_EFD_NONBLOCK  PLASMA_O_NONBLOCK
#define PLASMA_EFD_CLOEXEC   PLASMA_SOCK_CLOEXEC
#define PLASMA_EVENTFD_MAX   (UINT64_MAX - 1ull)

struct plasma_eventfd_object {
    bool used;
    bool semaphore;
    uint64_t counter;
};
'''
    text = rep(text, pipe_struct, eventfd_struct)

    text = rep(text,
               "static struct plasma_pipe_object plasma_pipes[PLASMA_PIPE_OBJECTS];\n",
               "static struct plasma_pipe_object plasma_pipes[PLASMA_PIPE_OBJECTS];\n"
               "static struct plasma_eventfd_object plasma_eventfds[PLASMA_EVENTFD_OBJECTS];\n")
    text = rep(text,
               "    bytes_zero(plasma_pipes, sizeof(plasma_pipes));\n",
               "    bytes_zero(plasma_pipes, sizeof(plasma_pipes));\n"
               "    bytes_zero(plasma_eventfds, sizeof(plasma_eventfds));\n")

    # Forward declarations: low-level read/write/close and execve occur before
    # the scheduler/process table where the cooperative lifetime code is defined.
    read_anchor = "static int64_t plasma_runtime_read(int fd, uint64_t address, uint64_t length) {\n"
    declarations = (
        "static void plasma_eventfd_progress(uint16_t object);\n"
        "static void plasma_reclaim_eventfd_if_unused(uint16_t object);\n"
        "static void plasma_close_cloexec_eventfd_fds(void);\n\n"
    )
    text = rep(text, read_anchor, declarations + read_anchor)

    # Counter read: eventfd reads one 64-bit value. In semaphore mode one token
    # is consumed; otherwise the whole current counter is returned and cleared.
    pipe_read_anchor = "    if (entry->type == PLASMA_RT_PIPE_R) {\n"
    eventfd_read = r'''    if (entry->type == PLASMA_RT_EVENTFD) {
        if (entry->object >= PLASMA_EVENTFD_OBJECTS ||
            !plasma_eventfds[entry->object].used)
            return -LINUX_EBADF;
        if (length < sizeof(uint64_t)) return -LINUX_EINVAL;
        struct plasma_eventfd_object *event = &plasma_eventfds[entry->object];
        if (event->counter == 0) return -LINUX_EAGAIN;
        const uint64_t value = event->semaphore ? 1ull : event->counter;
        if (!user_copy_out(address, &value, sizeof(value))) return -LINUX_EFAULT;
        if (event->semaphore) --event->counter;
        else event->counter = 0;
        plasma_eventfd_progress(entry->object);
        return (int64_t)sizeof(value);
    }
'''
    text = rep(text, pipe_read_anchor, eventfd_read + pipe_read_anchor)

    # Counter write: UINT64_MAX is forbidden. If the addition would exceed
    # UINT64_MAX-1, report EAGAIN; the syscall wrapper below turns that into a
    # cooperative sleep for blocking eventfds.
    pipe_write_anchor = "    if (entry->type == PLASMA_RT_PIPE_W) {\n"
    eventfd_write = r'''    if (entry->type == PLASMA_RT_EVENTFD) {
        if (entry->object >= PLASMA_EVENTFD_OBJECTS ||
            !plasma_eventfds[entry->object].used)
            return -LINUX_EBADF;
        if (length < sizeof(uint64_t)) return -LINUX_EINVAL;
        uint64_t value = 0;
        if (!user_copy_in(&value, address, sizeof(value))) return -LINUX_EFAULT;
        if (value == UINT64_MAX) return -LINUX_EINVAL;
        struct plasma_eventfd_object *event = &plasma_eventfds[entry->object];
        if (value > PLASMA_EVENTFD_MAX - event->counter) return -LINUX_EAGAIN;
        event->counter += value;
        plasma_eventfd_progress(entry->object);
        return (int64_t)sizeof(value);
    }
'''
    text = rep(text, pipe_write_anchor, eventfd_write + pipe_write_anchor)

    # Closing the last eventfd descriptor/waiting syscall releases its counter
    # object. Pipe lifetime bookkeeping remains untouched.
    close_old = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const bool was_pipe = (entry->type == PLASMA_RT_PIPE_R ||
                           entry->type == PLASMA_RT_PIPE_W) &&
                          entry->object < PLASMA_PIPE_OBJECTS;
    const bool was_pipe_writer = entry->type == PLASMA_RT_PIPE_W &&
                                 entry->object < PLASMA_PIPE_OBJECTS;
    const uint16_t pipe_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    if (was_pipe) plasma_reclaim_pipe_if_unused(pipe_object);
    return 0;
}
'''
    close_new = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const bool was_pipe = (entry->type == PLASMA_RT_PIPE_R ||
                           entry->type == PLASMA_RT_PIPE_W) &&
                          entry->object < PLASMA_PIPE_OBJECTS;
    const bool was_pipe_writer = entry->type == PLASMA_RT_PIPE_W &&
                                 entry->object < PLASMA_PIPE_OBJECTS;
    const bool was_eventfd = entry->type == PLASMA_RT_EVENTFD &&
                             entry->object < PLASMA_EVENTFD_OBJECTS;
    const uint16_t pipe_object = entry->object;
    const uint16_t eventfd_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    if (was_pipe) plasma_reclaim_pipe_if_unused(pipe_object);
    if (was_eventfd) plasma_reclaim_eventfd_if_unused(eventfd_object);
    return 0;
}
'''
    text = rep(text, close_old, close_new)

    # Allocate/create transactionally. Semaphore is an object property; the fd
    # stores only descriptor/file-status flags used by fcntl and close-on-exec.
    socket_alloc_anchor = "static int plasma_alloc_socket_object(void) {\n"
    creation = r'''static int plasma_alloc_eventfd_object(void) {
    for (int i = 0; i < PLASMA_EVENTFD_OBJECTS; ++i) {
        if (plasma_eventfds[i].used) continue;
        bytes_zero(&plasma_eventfds[i], sizeof(plasma_eventfds[i]));
        plasma_eventfds[i].used = true;
        return i;
    }
    return -1;
}

static int64_t plasma_eventfd_create(uint32_t initial_value, uint32_t flags) {
    const uint32_t allowed = PLASMA_EFD_SEMAPHORE |
                             PLASMA_EFD_NONBLOCK |
                             PLASMA_EFD_CLOEXEC;
    if ((flags & ~allowed) != 0) return -LINUX_EINVAL;
    plasma_runtime_init();

    const int object = plasma_alloc_eventfd_object();
    if (object < 0) return -LINUX_ENFILE;
    plasma_eventfds[object].counter = initial_value;
    plasma_eventfds[object].semaphore = (flags & PLASMA_EFD_SEMAPHORE) != 0;

    const uint32_t fd_flags = flags & (PLASMA_EFD_NONBLOCK | PLASMA_EFD_CLOEXEC);
    const int fd = plasma_alloc_runtime_fd(PLASMA_RT_EVENTFD,
                                           (uint16_t)object, fd_flags);
    if (fd < 0) {
        bytes_zero(&plasma_eventfds[object], sizeof(plasma_eventfds[object]));
        return fd;
    }
    return fd;
}

'''
    text = rep(text, socket_alloc_anchor, creation + socket_alloc_anchor)

    # eventfd poll/epoll readiness. Normal runtime objects remain writable as
    # before; an eventfd becomes non-writable only at UINT64_MAX-1.
    text = rep(text,
               "    if ((events & POLLOUT) != 0 && entry->type != PLASMA_RT_DIR) *revents |= POLLOUT;\n",
               r'''    if ((events & POLLOUT) != 0) {
        if (entry->type == PLASMA_RT_EVENTFD) {
            if (entry->object < PLASMA_EVENTFD_OBJECTS &&
                plasma_eventfds[entry->object].used &&
                plasma_eventfds[entry->object].counter < PLASMA_EVENTFD_MAX)
                *revents |= POLLOUT;
        } else if (entry->type != PLASMA_RT_DIR) {
            *revents |= POLLOUT;
        }
    }
''')
    text = rep(text,
               "        if (entry->type == PLASMA_RT_FILE) *revents |= POLLIN;\n"
               "        else if (entry->type == PLASMA_RT_PIPE_R) {\n",
               "        if (entry->type == PLASMA_RT_FILE) *revents |= POLLIN;\n"
               "        else if (entry->type == PLASMA_RT_EVENTFD) {\n"
               "            if (entry->object < PLASMA_EVENTFD_OBJECTS &&\n"
               "                plasma_eventfds[entry->object].used &&\n"
               "                plasma_eventfds[entry->object].counter != 0)\n"
               "                *revents |= POLLIN;\n"
               "        } else if (entry->type == PLASMA_RT_PIPE_R) {\n")

    # Track a blocked eventfd syscall distinctly from pipe/socket waits.
    text = rep(text,
               "    uint16_t blocked_socket_object;\n"
               "    uint16_t blocked_poll_socket_object;\n",
               "    uint16_t blocked_socket_object;\n"
               "    uint16_t blocked_eventfd_object;\n"
               "    bool blocked_eventfd_write;\n"
               "    uint64_t blocked_eventfd_value;\n"
               "    uint16_t blocked_poll_socket_object;\n")

    # Helpers are inserted after pthread support, so descriptor scans can resolve
    # CLONE_FILES threads through their canonical thread-group leader.
    exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    helpers = r'''static bool plasma_fd_table_has_eventfd(
        const struct plasma_runtime_fd *table, size_t count, uint16_t object) {
    if (table == 0) return false;
    for (size_t i = 0; i < count; ++i)
        if (table[i].used && table[i].type == PLASMA_RT_EVENTFD &&
            table[i].object == object)
            return true;
    return false;
}

static bool plasma_eventfd_has_live_reference(uint16_t object) {
    if (object >= PLASMA_EVENTFD_OBJECTS || !plasma_eventfds[object].used)
        return false;
    struct plasma_process *current = plasma_current_process();

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;

        if (process->state == PLASMA_PROC_BLOCKED_IO &&
            process->blocked_pipe_object == UINT16_MAX &&
            process->blocked_socket_object == UINT16_MAX &&
            process->blocked_eventfd_object == object)
            return true;

        if (process == current) {
            if (plasma_fd_table_has_eventfd(plasma_runtime_fds,
                    PLASMA_RUNTIME_FD_COUNT, object) ||
                plasma_fd_table_has_eventfd(plasma_low_runtime_fds, 10u, object))
                return true;
            continue;
        }

        struct plasma_process *owner = plasma_thread_group_leader(process);
        if (owner == 0) owner = process;
        if (plasma_fd_table_has_eventfd(owner->runtime_fds,
                PLASMA_RUNTIME_FD_COUNT, object) ||
            plasma_fd_table_has_eventfd(owner->low_runtime_fds, 10u, object))
            return true;
    }
    return false;
}

static void plasma_reclaim_eventfd_if_unused(uint16_t object) {
    if (object >= PLASMA_EVENTFD_OBJECTS || !plasma_eventfds[object].used) return;
    if (!plasma_eventfd_has_live_reference(object))
        bytes_zero(&plasma_eventfds[object], sizeof(plasma_eventfds[object]));
}

static void plasma_reclaim_dead_eventfds(void) {
    for (uint16_t object = 0; object < PLASMA_EVENTFD_OBJECTS; ++object)
        if (plasma_eventfds[object].used)
            plasma_reclaim_eventfd_if_unused(object);
}

static void plasma_close_cloexec_eventfd_fds(void) {
    for (int fd = 0; fd < 10; ++fd) {
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && entry->type == PLASMA_RT_EVENTFD &&
            (entry->flags & PLASMA_EFD_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
    for (int i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        const int fd = PLASMA_RUNTIME_FD_FIRST + i;
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && entry->type == PLASMA_RT_EVENTFD &&
            (entry->flags & PLASMA_EFD_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
}

static int64_t plasma_cooperative_eventfd_read_result(int fd,
                                                       uint64_t address,
                                                       uint64_t length,
                                                       int64_t result) {
    if (result != -LINUX_EAGAIN) return result;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_EVENTFD ||
        entry->object >= PLASMA_EVENTFD_OBJECTS)
        return result;
    if ((entry->flags & PLASMA_EFD_NONBLOCK) != 0) return result;
    if (length < sizeof(uint64_t)) return -LINUX_EINVAL;

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return result;
    plasma_save_active(current);
    current->blocked_pipe_object = UINT16_MAX;
    current->blocked_socket_object = UINT16_MAX;
    current->blocked_eventfd_object = entry->object;
    current->blocked_eventfd_write = false;
    current->blocked_eventfd_value = 0;
    current->blocked_io_address = address;
    current->blocked_io_length = length;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_IO;
    return 0;
}

static int64_t plasma_cooperative_eventfd_write_result(int fd,
                                                        uint64_t address,
                                                        uint64_t length,
                                                        int64_t result) {
    if (result != -LINUX_EAGAIN) return result;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_EVENTFD ||
        entry->object >= PLASMA_EVENTFD_OBJECTS)
        return result;
    if ((entry->flags & PLASMA_EFD_NONBLOCK) != 0) return result;
    if (length < sizeof(uint64_t)) return -LINUX_EINVAL;

    uint64_t value = 0;
    if (!user_copy_in(&value, address, sizeof(value))) return -LINUX_EFAULT;
    if (value == UINT64_MAX) return -LINUX_EINVAL;

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return result;
    plasma_save_active(current);
    current->blocked_pipe_object = UINT16_MAX;
    current->blocked_socket_object = UINT16_MAX;
    current->blocked_eventfd_object = entry->object;
    current->blocked_eventfd_write = true;
    current->blocked_eventfd_value = value;
    current->blocked_io_address = 0;
    current->blocked_io_length = sizeof(uint64_t);
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_IO;
    return 0;
}

static void plasma_eventfd_finish_waiter(struct plasma_process *waiter,
                                         int64_t result) {
    waiter->pending_result = result;
    waiter->blocked_pipe_object = 0;
    waiter->blocked_socket_object = 0;
    waiter->blocked_eventfd_object = 0;
    waiter->blocked_eventfd_write = false;
    waiter->blocked_eventfd_value = 0;
    waiter->blocked_io_address = 0;
    waiter->blocked_io_length = 0;
    waiter->state = PLASMA_PROC_RUNNABLE;
}

static void plasma_eventfd_progress(uint16_t object) {
    if (object >= PLASMA_EVENTFD_OBJECTS || !plasma_eventfds[object].used) return;
    struct plasma_eventfd_object *event = &plasma_eventfds[object];

    /* Alternate between readable and writable waiters until no waiter can make
     * progress. This naturally handles EFD_SEMAPHORE token-by-token wakeups. */
    for (;;) {
        bool progressed = false;

        if (event->counter != 0) {
            for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
                struct plasma_process *reader = &plasma_processes[i];
                if (reader->state != PLASMA_PROC_BLOCKED_IO ||
                    reader->blocked_pipe_object != UINT16_MAX ||
                    reader->blocked_socket_object != UINT16_MAX ||
                    reader->blocked_eventfd_object != object ||
                    reader->blocked_eventfd_write)
                    continue;

                const uint64_t value = event->semaphore ? 1ull : event->counter;
                bytes_copy(&plasma_scheduler_scratch_image, &image, sizeof(image));
                bytes_copy(&image, &reader->image, sizeof(image));
                const bool stored = user_copy_out(reader->blocked_io_address,
                                                  &value, sizeof(value));
                bytes_copy(&image, &plasma_scheduler_scratch_image, sizeof(image));
                if (!stored) {
                    plasma_eventfd_finish_waiter(reader, -LINUX_EFAULT);
                    progressed = true;
                    break;
                }

                if (event->semaphore) --event->counter;
                else event->counter = 0;
                plasma_eventfd_finish_waiter(reader, (int64_t)sizeof(uint64_t));
                progressed = true;
                break;
            }
            if (progressed) continue;
        }

        for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
            struct plasma_process *writer = &plasma_processes[i];
            if (writer->state != PLASMA_PROC_BLOCKED_IO ||
                writer->blocked_pipe_object != UINT16_MAX ||
                writer->blocked_socket_object != UINT16_MAX ||
                writer->blocked_eventfd_object != object ||
                !writer->blocked_eventfd_write)
                continue;
            const uint64_t value = writer->blocked_eventfd_value;
            if (value > PLASMA_EVENTFD_MAX - event->counter) continue;
            event->counter += value;
            plasma_eventfd_finish_waiter(writer, (int64_t)sizeof(uint64_t));
            progressed = true;
            break;
        }

        if (!progressed) break;
    }
    plasma_reclaim_eventfd_if_unused(object);
}

'''
    text = rep(text, exit_anchor, helpers + exit_anchor)

    # Common read dispatch: pipe and socket wrappers get first chance, then
    # eventfd converts a zero-counter EAGAIN into a cooperative block.
    read_old = (
        "            const int64_t pipe_result = plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (pipe_result != runtime_result) return pipe_result;\n"
        "            return plasma_cooperative_socket_read_result((int)a1, a2, a3, runtime_result);\n"
    )
    read_new = (
        "            const int64_t pipe_result = plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (pipe_result != runtime_result) return pipe_result;\n"
        "            const int64_t socket_result = plasma_cooperative_socket_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (socket_result != runtime_result) return socket_result;\n"
        "            return plasma_cooperative_eventfd_read_result((int)a1, a2, a3, runtime_result);\n"
    )
    text = rep(text, read_old, read_new)

    # Ordinary write(2) gets blocking overflow semantics. The existing generic
    # writev implementation remains a sequence of runtime writes; eventfd users
    # normally use the required 8-byte write interface, and this patch does not
    # pretend to redesign the broader vectored-I/O layer.
    write_old = (
        "    case SYS_WRITE:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_write((int)a1, a2, a3);\n"
    )
    write_new = (
        "    case SYS_WRITE:\n"
        "        if (plasma_runtime_fd((int)a1) != 0) {\n"
        "            const int64_t runtime_result = plasma_runtime_write((int)a1, a2, a3);\n"
        "            return plasma_cooperative_eventfd_write_result((int)a1, a2, a3, runtime_result);\n"
        "        }\n"
    )
    text = rep(text, write_old, write_new)

    # Syscall dispatch.
    dispatch_anchor = "    case SYS_EPOLL_CREATE1: return plasma_epoll_create1((uint32_t)a1);\n"
    text = rep(text, dispatch_anchor,
               "    case SYS_EVENTFD: return plasma_eventfd_create((uint32_t)a1, 0);\n"
               "    case SYS_EVENTFD2: return plasma_eventfd_create((uint32_t)a1, (uint32_t)a2);\n"
               + dispatch_anchor)

    # Apply eventfd CLOEXEC only after successful new-image construction. Pipe
    # CLOEXEC is handled independently by the pipe-lifetime finalizer.
    text = rep(text,
               "    plasma_close_cloexec_pipe_fds();\n"
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n",
               "    plasma_close_cloexec_pipe_fds();\n"
               "    plasma_close_cloexec_eventfd_fds();\n"
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n")

    # A whole process becoming a zombie drops its descriptor table. Thread exit
    # returns before this point because CLONE_FILES belongs to the group leader.
    text = rep(text,
               "    plasma_reclaim_dead_pipes();\n"
               "    plasma_wake_waiters_for(process);\n",
               "    plasma_reclaim_dead_pipes();\n"
               "    plasma_reclaim_dead_eventfds();\n"
               "    plasma_wake_waiters_for(process);\n")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized real eventfd/eventfd2 counter + wait/lifetime semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
