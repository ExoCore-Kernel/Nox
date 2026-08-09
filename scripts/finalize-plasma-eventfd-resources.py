#!/usr/bin/env python3
"""Implement GLib eventfd2 and correct runtime pipe/resource lifetime semantics.

The Plasma bring-up reached GLib's GWakeup implementation.  Twilight returned
ENOSYS for eventfd2, so GLib fell back to pipe2.  The pipe fallback then exposed
a real lifetime bug: closing a runtime descriptor cleared only the fd slot and
never reclaimed its pipe object.  pipe2 failures also leaked partially-created
state and O_CLOEXEC was stored but not applied at execve.

This finalizer fixes the underlying runtime semantics instead of merely raising
fixed table limits:

* eventfd/eventfd2 provide shared 64-bit counter objects, EFD_SEMAPHORE,
  EFD_NONBLOCK, EFD_CLOEXEC, read/write and poll/epoll readiness;
* blocking eventfd reads use the existing cooperative scheduler and are woken by
  writes, while nonblocking reads return EAGAIN;
* pipe2 creation is transactional and reports EMFILE/ENFILE rather than leaking;
* pipe objects are reclaimed after the final live endpoint disappears, including
  inherited/duplicated descriptors across processes;
* writes with no live pipe reader return EPIPE;
* successful execve closes runtime descriptors marked CLOEXEC;
* process exit reclaims runtime pipe/eventfd objects that became unreachable.

The descriptor/object tables remain finite kernel resource tables.  Their limits
are increased from tiny bring-up values to practical values, but exhaustion is
reported with Linux errors and is not hidden by fake success.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected eventfd/resource fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Practical kernel resource limits. These are still finite and return the
    # appropriate Linux exhaustion errors instead of pretending allocation
    # succeeded. eventfd avoids consuming a pipe for every GLib GWakeup.
    text = rep(text, "#define PLASMA_RUNTIME_FD_COUNT 48\n",
               "#define PLASMA_RUNTIME_FD_COUNT 128\n")
    text = rep(text, "#define PLASMA_PIPE_OBJECTS 16\n",
               "#define PLASMA_PIPE_OBJECTS 64\n")
    text = rep(text, "#define PLASMA_SOCKET_OBJECTS 24\n",
               "#define PLASMA_SOCKET_OBJECTS 64\n")

    # Linux syscall numbers and precise fd-table exhaustion errors.
    text = rep(text,
               "#define SYS_EPOLL_PWAIT    281ull\n",
               "#define SYS_EPOLL_PWAIT    281ull\n#define SYS_EVENTFD        284ull\n")
    text = rep(text,
               "#define SYS_EPOLL_CREATE1  291ull\n",
               "#define SYS_EVENTFD2       290ull\n#define SYS_EPOLL_CREATE1  291ull\n")
    if "#define LINUX_ENFILE" not in text:
        text = rep(text,
                   "#define LINUX_ENOTDIR    20\n",
                   "#define LINUX_ENOTDIR    20\n#define LINUX_ENFILE      23\n#define LINUX_EMFILE      24\n")

    # Type 6 is epoll and type 7 is inotify in the current generated ABI.
    text = rep(text,
               "#define PLASMA_RT_INOTIFY 7u\n",
               "#define PLASMA_RT_INOTIFY 7u\n#define PLASMA_RT_EVENTFD 8u\n")

    # Add the eventfd object beside the other kernel-backed runtime objects.
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
    uint32_t flags;
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

    # A full runtime descriptor table is a per-process exhaustion condition.
    text = rep(text,
               "    return -LINUX_EBUSY;\n}\n\nstatic bool plasma_runtime_path",
               "    return -LINUX_EMFILE;\n}\n\nstatic bool plasma_runtime_path")

    # Lifetime helpers are implemented later (after the process/thread model),
    # but close/read/write are emitted before that model.
    close_anchor = "static int64_t plasma_close_runtime(int fd) {\n"
    text = rep(text, close_anchor,
               "static bool plasma_pipe_has_live_reader(uint16_t object);\n"
               "static bool plasma_runtime_object_has_live_descriptor(uint8_t type, uint16_t object);\n"
               "static void plasma_runtime_reclaim_object(uint8_t type, uint16_t object);\n"
               "static void plasma_close_cloexec_runtime_fds(void);\n\n" + close_anchor)

    # eventfd write wakes cooperative readers. The declaration must precede the
    # common runtime write helper used by write/writev/sendmsg.
    notify_anchor = (
        "static void plasma_runtime_notify_socket_data(uint16_t object);\n\n"
        "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n"
    )
    text = rep(text, notify_anchor,
               "static void plasma_runtime_notify_socket_data(uint16_t object);\n"
               "static void plasma_runtime_notify_eventfd(uint16_t object);\n\n"
               "static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {\n")

    # Real eventfd read semantics. A blocking zero-counter read is converted to
    # a scheduler sleep by a later wrapper; this low-level helper returns EAGAIN
    # as the readiness indication, just like a nonblocking file operation.
    pipe_read_anchor = "    if (entry->type == PLASMA_RT_PIPE_R) {\n"
    eventfd_read = r'''    if (entry->type == PLASMA_RT_EVENTFD) {
        if (entry->object >= PLASMA_EVENTFD_OBJECTS ||
            !plasma_eventfds[entry->object].used)
            return -LINUX_EBADF;
        if (length < sizeof(uint64_t)) return -LINUX_EINVAL;
        struct plasma_eventfd_object *event = &plasma_eventfds[entry->object];
        if (event->counter == 0) return -LINUX_EAGAIN;
        const uint64_t value = (event->flags & PLASMA_EFD_SEMAPHORE) != 0 ?
                               1ull : event->counter;
        if (!user_copy_out(address, &value, sizeof(value))) return -LINUX_EFAULT;
        if ((event->flags & PLASMA_EFD_SEMAPHORE) != 0) --event->counter;
        else event->counter = 0;
        return (int64_t)sizeof(value);
    }
'''
    text = rep(text, pipe_read_anchor, eventfd_read + pipe_read_anchor)

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
        plasma_runtime_notify_eventfd(entry->object);
        return (int64_t)sizeof(value);
    }
'''
    text = rep(text, pipe_write_anchor, eventfd_write + pipe_write_anchor)

    # A pipe write with no read endpoint is EPIPE. SIGPIPE delivery is a signal
    # subsystem concern; the file operation itself must still return EPIPE.
    text = rep(text,
               "    if (entry->type == PLASMA_RT_PIPE_W) {\n"
               "        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];\n",
               "    if (entry->type == PLASMA_RT_PIPE_W) {\n"
               "        if (!plasma_pipe_has_live_reader(entry->object)) return -LINUX_EPIPE;\n"
               "        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];\n")

    # close(2): preserve the proven pipe EOF wake, then reclaim a pipe/eventfd
    # only after no live duplicated/inherited descriptor (or blocked read) still
    # references the object.
    close_old = r'''static int64_t plasma_close_runtime(int fd) {
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
    close_new = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const uint8_t old_type = entry->type;
    const uint16_t old_object = entry->object;
    const bool was_pipe_writer = old_type == PLASMA_RT_PIPE_W &&
                                 old_object < PLASMA_PIPE_OBJECTS;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(old_object))
        plasma_runtime_notify_pipe_eof(old_object);
    plasma_runtime_reclaim_object(old_type, old_object);
    return 0;
}
'''
    text = rep(text, close_old, close_new)

    # pipe2 creation must be atomic from userspace's perspective. Roll back the
    # object and either fd on every failure path.
    pipe2_old = r'''static int64_t plasma_pipe2(uint64_t pair_address, uint32_t flags) {
    plasma_runtime_init();
    int object = plasma_alloc_pipe_object();
    if (object < 0) return -LINUX_ENOMEM;
    int read_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_R, (uint16_t)object, flags);
    int write_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_W, (uint16_t)object, flags);
    if (read_fd < 0 || write_fd < 0) return -LINUX_EBUSY;
    int32_t pair[2] = { read_fd, write_fd };
    return user_copy_out(pair_address, pair, sizeof(pair)) ? 0 : -LINUX_EFAULT;
}
'''
    pipe2_new = r'''static int64_t plasma_pipe2(uint64_t pair_address, uint32_t flags) {
    plasma_runtime_init();
    const uint32_t allowed = PLASMA_O_NONBLOCK | PLASMA_SOCK_CLOEXEC;
    if ((flags & ~allowed) != 0) return -LINUX_EINVAL;

    int object = plasma_alloc_pipe_object();
    if (object < 0) return -LINUX_ENFILE;

    int read_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_R, (uint16_t)object, flags);
    if (read_fd < 0) {
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return read_fd;
    }
    int write_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_W, (uint16_t)object, flags);
    if (write_fd < 0) {
        struct plasma_runtime_fd *read_entry = plasma_runtime_fd(read_fd);
        if (read_entry != 0) bytes_zero(read_entry, sizeof(*read_entry));
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return write_fd;
    }

    int32_t pair[2] = { read_fd, write_fd };
    if (!user_copy_out(pair_address, pair, sizeof(pair))) {
        struct plasma_runtime_fd *read_entry = plasma_runtime_fd(read_fd);
        struct plasma_runtime_fd *write_entry = plasma_runtime_fd(write_fd);
        if (read_entry != 0) bytes_zero(read_entry, sizeof(*read_entry));
        if (write_entry != 0) bytes_zero(write_entry, sizeof(*write_entry));
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return -LINUX_EFAULT;
    }
    return 0;
}
'''
    text = rep(text, pipe2_old, pipe2_new)

    # Eventfd object allocation and syscall entry points.
    socket_alloc_anchor = "static int plasma_alloc_socket_object(void) {\n"
    eventfd_alloc = r'''static int plasma_alloc_eventfd_object(void) {
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
    int object = plasma_alloc_eventfd_object();
    if (object < 0) return -LINUX_ENFILE;
    plasma_eventfds[object].counter = initial_value;
    plasma_eventfds[object].flags = flags;
    int fd = plasma_alloc_runtime_fd(PLASMA_RT_EVENTFD, (uint16_t)object, flags);
    if (fd < 0) {
        bytes_zero(&plasma_eventfds[object], sizeof(plasma_eventfds[object]));
        return fd;
    }
    return fd;
}

'''
    text = rep(text, socket_alloc_anchor, eventfd_alloc + socket_alloc_anchor)

    # Poll/epoll readiness for eventfd counters. A counter is readable when
    # nonzero and writable while at least value 1 could be added.
    poll_out_old = "    if ((events & POLLOUT) != 0 && entry->type != PLASMA_RT_DIR) *revents |= POLLOUT;\n"
    poll_out_new = r'''    if ((events & POLLOUT) != 0) {
        if (entry->type == PLASMA_RT_EVENTFD) {
            if (entry->object < PLASMA_EVENTFD_OBJECTS &&
                plasma_eventfds[entry->object].used &&
                plasma_eventfds[entry->object].counter < PLASMA_EVENTFD_MAX)
                *revents |= POLLOUT;
        } else if (entry->type != PLASMA_RT_DIR) {
            *revents |= POLLOUT;
        }
    }
'''
    text = rep(text, poll_out_old, poll_out_new)
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

    # One wait identity for eventfd blocking reads alongside pipe/socket waits.
    text = rep(text,
               "    uint16_t blocked_socket_object;\n"
               "    uint64_t blocked_poll_address;\n",
               "    uint16_t blocked_socket_object;\n"
               "    uint16_t blocked_eventfd_object;\n"
               "    uint64_t blocked_poll_address;\n")

    # Resource/reference and cooperative eventfd helpers live after the thread
    # model so CLONE_FILES threads can resolve their canonical group leader.
    process_exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    resource_helpers = r'''static bool plasma_fd_table_references_runtime_object(
        const struct plasma_runtime_fd *table, size_t count,
        uint8_t type, uint16_t object) {
    if (table == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!table[i].used) continue;
        if (type == PLASMA_RT_PIPE_R || type == PLASMA_RT_PIPE_W) {
            if ((table[i].type == PLASMA_RT_PIPE_R || table[i].type == PLASMA_RT_PIPE_W) &&
                table[i].object == object)
                return true;
        } else if (table[i].type == type && table[i].object == object) {
            return true;
        }
    }
    return false;
}

static bool plasma_runtime_object_has_live_descriptor(uint8_t type, uint16_t object) {
    struct plasma_process *current = plasma_current_process();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;

        if (process->state == PLASMA_PROC_BLOCKED_IO) {
            if ((type == PLASMA_RT_PIPE_R || type == PLASMA_RT_PIPE_W) &&
                process->blocked_pipe_object == object)
                return true;
            if (type == PLASMA_RT_EVENTFD && process->blocked_eventfd_object == object)
                return true;
        }

        if (process == current) {
            if (plasma_fd_table_references_runtime_object(plasma_runtime_fds,
                    PLASMA_RUNTIME_FD_COUNT, type, object) ||
                plasma_fd_table_references_runtime_object(plasma_low_runtime_fds,
                    10u, type, object))
                return true;
            continue;
        }

        struct plasma_process *owner = plasma_thread_group_leader(process);
        if (owner == 0) owner = process;
        if (plasma_fd_table_references_runtime_object(owner->runtime_fds,
                PLASMA_RUNTIME_FD_COUNT, type, object) ||
            plasma_fd_table_references_runtime_object(owner->low_runtime_fds,
                10u, type, object))
            return true;
    }
    return false;
}

static bool plasma_pipe_has_live_reader(uint16_t object) {
    if (object >= PLASMA_PIPE_OBJECTS || !plasma_pipes[object].used) return false;
    struct plasma_process *current = plasma_current_process();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;
        if (process->state == PLASMA_PROC_BLOCKED_IO &&
            process->blocked_pipe_object == object)
            return true;
        struct plasma_process *owner = process == current ? process :
                                       plasma_thread_group_leader(process);
        if (owner == 0) owner = process;
        const struct plasma_runtime_fd *high = process == current ?
            plasma_runtime_fds : owner->runtime_fds;
        const struct plasma_runtime_fd *low = process == current ?
            plasma_low_runtime_fds : owner->low_runtime_fds;
        for (size_t j = 0; j < PLASMA_RUNTIME_FD_COUNT; ++j)
            if (high[j].used && high[j].type == PLASMA_RT_PIPE_R &&
                high[j].object == object) return true;
        for (size_t j = 0; j < 10u; ++j)
            if (low[j].used && low[j].type == PLASMA_RT_PIPE_R &&
                low[j].object == object) return true;
    }
    return false;
}

static void plasma_runtime_reclaim_object(uint8_t type, uint16_t object) {
    if (type == PLASMA_RT_PIPE_R || type == PLASMA_RT_PIPE_W) {
        if (object < PLASMA_PIPE_OBJECTS && plasma_pipes[object].used &&
            !plasma_runtime_object_has_live_descriptor(PLASMA_RT_PIPE_R, object))
            bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return;
    }
    if (type == PLASMA_RT_EVENTFD) {
        if (object < PLASMA_EVENTFD_OBJECTS && plasma_eventfds[object].used &&
            !plasma_runtime_object_has_live_descriptor(PLASMA_RT_EVENTFD, object))
            bytes_zero(&plasma_eventfds[object], sizeof(plasma_eventfds[object]));
    }
}

static void plasma_runtime_reclaim_dead_objects(void) {
    for (uint16_t object = 0; object < PLASMA_PIPE_OBJECTS; ++object)
        if (plasma_pipes[object].used)
            plasma_runtime_reclaim_object(PLASMA_RT_PIPE_R, object);
    for (uint16_t object = 0; object < PLASMA_EVENTFD_OBJECTS; ++object)
        if (plasma_eventfds[object].used)
            plasma_runtime_reclaim_object(PLASMA_RT_EVENTFD, object);
}

static void plasma_close_cloexec_runtime_fds(void) {
    for (int fd = 0; fd < 10; ++fd) {
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
    for (int i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        const int fd = PLASMA_RUNTIME_FD_FIRST + i;
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
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
    current->blocked_io_address = address;
    current->blocked_io_length = length;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_IO;
    return 0;
}

static void plasma_runtime_notify_eventfd(uint16_t object) {
    if (object >= PLASMA_EVENTFD_OBJECTS || !plasma_eventfds[object].used) return;
    struct plasma_eventfd_object *event = &plasma_eventfds[object];

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES && event->counter != 0; ++i) {
        struct plasma_process *reader = &plasma_processes[i];
        if (reader->state != PLASMA_PROC_BLOCKED_IO ||
            reader->blocked_eventfd_object != object)
            continue;

        const uint64_t value = (event->flags & PLASMA_EFD_SEMAPHORE) != 0 ?
                               1ull : event->counter;
        bytes_copy(&plasma_scheduler_scratch_image, &image, sizeof(image));
        bytes_copy(&image, &reader->image, sizeof(image));
        const bool stored = user_copy_out(reader->blocked_io_address,
                                          &value, sizeof(value));
        bytes_copy(&image, &plasma_scheduler_scratch_image, sizeof(image));

        if (stored) {
            if ((event->flags & PLASMA_EFD_SEMAPHORE) != 0) --event->counter;
            else event->counter = 0;
            reader->pending_result = (int64_t)sizeof(value);
        } else {
            reader->pending_result = -LINUX_EFAULT;
        }
        reader->blocked_pipe_object = 0;
        reader->blocked_socket_object = 0;
        reader->blocked_eventfd_object = 0;
        reader->blocked_io_address = 0;
        reader->blocked_io_length = 0;
        reader->state = PLASMA_PROC_RUNNABLE;
    }
}

'''
    text = rep(text, process_exit_anchor, resource_helpers + process_exit_anchor)

    # Feed eventfd's EAGAIN through the cooperative blocking-read path after the
    # proven pipe/socket wrappers have had first chance to handle their types.
    read_dispatch_old = (
        "            const int64_t pipe_result = plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (pipe_result != runtime_result) return pipe_result;\n"
        "            return plasma_cooperative_socket_read_result((int)a1, a2, a3, runtime_result);\n"
    )
    read_dispatch_new = (
        "            const int64_t pipe_result = plasma_cooperative_pipe_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (pipe_result != runtime_result) return pipe_result;\n"
        "            const int64_t socket_result = plasma_cooperative_socket_read_result((int)a1, a2, a3, runtime_result);\n"
        "            if (socket_result != runtime_result) return socket_result;\n"
        "            return plasma_cooperative_eventfd_read_result((int)a1, a2, a3, runtime_result);\n"
    )
    text = rep(text, read_dispatch_old, read_dispatch_new)

    # eventfd/eventfd2 syscall dispatch. eventfd is the legacy no-flags form.
    dispatch_anchor = "    case SYS_EPOLL_CREATE1: return plasma_epoll_create1((uint32_t)a1);\n"
    eventfd_dispatch = (
        "    case SYS_EVENTFD: return plasma_eventfd_create((uint32_t)a1, 0);\n"
        "    case SYS_EVENTFD2: return plasma_eventfd_create((uint32_t)a1, (uint32_t)a2);\n"
    )
    text = rep(text, dispatch_anchor, eventfd_dispatch + dispatch_anchor)

    # Close CLOEXEC runtime descriptors only after the new image has been built
    # successfully. The older rootfs fd table remains handled by its existing
    # exec path; this fixes the runtime pipe/eventfd/socket descriptor class that
    # was previously never closed at exec at all.
    text = rep(text,
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n"
               "    linux_exec_set_return_state(image.entry, image.stack_pointer);\n",
               "    plasma_close_cloexec_runtime_fds();\n"
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n"
               "    linux_exec_set_return_state(image.entry, image.stack_pointer);\n")

    # Once a whole process becomes a zombie, its descriptor snapshots no longer
    # hold open-file references. Reclaim any pipe/eventfd objects that lost their
    # final live reference. Thread exits return earlier and preserve CLONE_FILES.
    text = rep(text,
               "    plasma_wake_waiters_for(process);\n"
               "    serial_write(\"[linux:process] process exited pid=\");\n",
               "    plasma_runtime_reclaim_dead_objects();\n"
               "    plasma_wake_waiters_for(process);\n"
               "    serial_write(\"[linux:process] process exited pid=\");\n")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized eventfd2 + transactional pipe/resource lifetime semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
