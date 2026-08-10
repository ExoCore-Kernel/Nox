#!/usr/bin/env python3
"""Implement real AF_UNIX peer credentials and endpoint lifetime for Plasma.

The D-Bus reference daemon authenticates Unix clients with SO_PEERCRED.  The
bring-up runtime previously returned four zero bytes for every getsockopt(),
which made struct ucred incomplete and caused AUTH EXTERNAL to be rejected.

This finalizer gives AF_UNIX stream endpoints persistent creator credentials,
snapshots peer credentials when a connection/socketpair is created, implements
SO_PEERCRED/SO_ERROR/SO_TYPE/SO_ACCEPTCONN getsockopt semantics, and gives
socket objects real last-reference lifetime/EOF/POLLHUP behavior.  It runs
after pthread, pipe-lifetime and eventfd finalizers so CLONE_FILES descriptor
sharing can be counted correctly.

Nox currently has a single Unix identity (uid=gid=0), so those values are not a
stub: they are the actual credentials returned by the existing getuid/getgid
ABI. PID is the process thread-group ID, matching Linux process credentials.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected AF_UNIX fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # A desktop session can have many simultaneous local sockets.  This remains
    # a finite kernel resource and allocation failure is still reported; unlike
    # the old table, closed endpoints below are actually reclaimed.
    text = rep(text, "#define PLASMA_SOCKET_OBJECTS 24\n",
               "#define PLASMA_SOCKET_OBJECTS 128\n")

    if "#define LINUX_ENOTSOCK" not in text:
        text = rep(text,
                   "#define LINUX_ENOSYS     38\n",
                   "#define LINUX_ENOSYS     38\n"
                   "#define LINUX_ENOTSOCK   88\n"
                   "#define LINUX_ENOPROTOOPT 92\n")

    text = rep(
        text,
        "#define PLASMA_SOCK_CLOEXEC 02000000u\n",
        "#define PLASMA_SOCK_CLOEXEC 02000000u\n"
        "#define PLASMA_SOL_SOCKET 1\n"
        "#define PLASMA_SO_TYPE 3\n"
        "#define PLASMA_SO_ERROR 4\n"
        "#define PLASMA_SO_PEERCRED 17\n"
        "#define PLASMA_SO_ACCEPTCONN 30\n",
    )

    socket_struct = r'''struct plasma_socket_object {
    bool used;
    bool listening;
    int peer;
    int pending;
    char path[108];
    uint8_t data[PLASMA_SOCKET_BUFFER];
    size_t head;
    size_t tail;
};
'''
    socket_struct_new = r'''struct plasma_ucred {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

struct plasma_socket_object {
    bool used;
    bool listening;
    bool peer_cred_valid;
    int peer;
    int pending;
    struct plasma_ucred owner_cred;
    struct plasma_ucred peer_cred;
    char path[108];
    uint8_t data[PLASMA_SOCKET_BUFFER];
    size_t head;
    size_t tail;
};
'''
    text = rep(text, socket_struct, socket_struct_new)

    # The socket allocator is emitted before the cooperative process table.
    # Resolve the current credentials through a forward-declared helper whose
    # implementation is inserted after the scheduler/thread structures exist.
    alloc_anchor = "static int plasma_alloc_socket_object(void) {\n"
    text = rep(text, alloc_anchor,
               "static void plasma_current_ucred(struct plasma_ucred *out);\n"
               "static void plasma_socket_peer_closed(uint16_t object);\n"
               "static void plasma_reclaim_socket_if_unused(uint16_t object);\n"
               "static void plasma_close_cloexec_socket_fds(void);\n\n" + alloc_anchor)

    alloc_old = r'''static int plasma_alloc_socket_object(void) {
    for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i) if (!plasma_sockets[i].used) {
        bytes_zero(&plasma_sockets[i], sizeof(plasma_sockets[i]));
        plasma_sockets[i].used = true;
        plasma_sockets[i].peer = -1;
        plasma_sockets[i].pending = -1;
        return i;
    }
    return -1;
}
'''
    alloc_new = r'''static int plasma_alloc_socket_object(void) {
    for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i) if (!plasma_sockets[i].used) {
        bytes_zero(&plasma_sockets[i], sizeof(plasma_sockets[i]));
        plasma_sockets[i].used = true;
        plasma_sockets[i].peer = -1;
        plasma_sockets[i].pending = -1;
        plasma_current_ucred(&plasma_sockets[i].owner_cred);
        return i;
    }
    return -1;
}
'''
    text = rep(text, alloc_old, alloc_new)

    # Snapshot credentials at connect time.  The server-side pending endpoint
    # belongs to the listener's process, not to the client that executes
    # connect(), so override the allocator's temporary owner accordingly.
    connect_old = r'''    int server = plasma_alloc_socket_object();
    if (server < 0) return -LINUX_ENOMEM;
    plasma_sockets[entry->object].peer = server;
    plasma_sockets[server].peer = entry->object;
    plasma_sockets[listener].pending = server;
'''
    connect_new = r'''    int server = plasma_alloc_socket_object();
    if (server < 0) return -LINUX_ENOMEM;
    struct plasma_socket_object *client_socket = &plasma_sockets[entry->object];
    struct plasma_socket_object *listener_socket = &plasma_sockets[listener];
    struct plasma_socket_object *server_socket = &plasma_sockets[server];

    server_socket->owner_cred = listener_socket->owner_cred;
    client_socket->peer_cred = server_socket->owner_cred;
    client_socket->peer_cred_valid = true;
    server_socket->peer_cred = client_socket->owner_cred;
    server_socket->peer_cred_valid = true;

    client_socket->peer = server;
    server_socket->peer = entry->object;
    listener_socket->pending = server;
'''
    text = rep(text, connect_old, connect_new)

    # socketpair endpoints are created by the same process, but still snapshot
    # credentials exactly as two connected AF_UNIX endpoints would.
    socketpair_old = r'''static int64_t plasma_socketpair(int domain, int type, int protocol, uint64_t pair_address) {
    (void)protocol;
    if (domain != PLASMA_AF_UNIX || (type & 0xf) != PLASMA_SOCK_STREAM) return -LINUX_EAFNOSUPPORT;
    int a = plasma_alloc_socket_object(), b = plasma_alloc_socket_object();
    if (a < 0 || b < 0) return -LINUX_ENOMEM;
    plasma_sockets[a].peer = b;
    plasma_sockets[b].peer = a;
    int fa = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)a, (uint32_t)type);
    int fb = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)b, (uint32_t)type);
    int32_t pair[2] = { fa, fb };
    return fa >= 0 && fb >= 0 && user_copy_out(pair_address, pair, sizeof(pair)) ? 0 : -LINUX_EFAULT;
}
'''
    socketpair_new = r'''static int64_t plasma_socketpair(int domain, int type, int protocol, uint64_t pair_address) {
    (void)protocol;
    if (domain != PLASMA_AF_UNIX || (type & 0xf) != PLASMA_SOCK_STREAM)
        return -LINUX_EAFNOSUPPORT;

    int a = plasma_alloc_socket_object();
    if (a < 0) return -LINUX_ENFILE;
    int b = plasma_alloc_socket_object();
    if (b < 0) {
        bytes_zero(&plasma_sockets[a], sizeof(plasma_sockets[a]));
        return -LINUX_ENFILE;
    }

    plasma_sockets[a].peer = b;
    plasma_sockets[b].peer = a;
    plasma_sockets[a].peer_cred = plasma_sockets[b].owner_cred;
    plasma_sockets[a].peer_cred_valid = true;
    plasma_sockets[b].peer_cred = plasma_sockets[a].owner_cred;
    plasma_sockets[b].peer_cred_valid = true;

    int fa = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)a, (uint32_t)type);
    if (fa < 0) {
        bytes_zero(&plasma_sockets[a], sizeof(plasma_sockets[a]));
        bytes_zero(&plasma_sockets[b], sizeof(plasma_sockets[b]));
        return fa;
    }
    int fb = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)b, (uint32_t)type);
    if (fb < 0) {
        struct plasma_runtime_fd *fa_entry = plasma_runtime_fd(fa);
        if (fa_entry != 0) bytes_zero(fa_entry, sizeof(*fa_entry));
        bytes_zero(&plasma_sockets[a], sizeof(plasma_sockets[a]));
        bytes_zero(&plasma_sockets[b], sizeof(plasma_sockets[b]));
        return fb;
    }

    const int32_t pair[2] = { fa, fb };
    if (!user_copy_out(pair_address, pair, sizeof(pair))) {
        struct plasma_runtime_fd *fa_entry = plasma_runtime_fd(fa);
        struct plasma_runtime_fd *fb_entry = plasma_runtime_fd(fb);
        if (fa_entry != 0) bytes_zero(fa_entry, sizeof(*fa_entry));
        if (fb_entry != 0) bytes_zero(fb_entry, sizeof(*fb_entry));
        bytes_zero(&plasma_sockets[a], sizeof(plasma_sockets[a]));
        bytes_zero(&plasma_sockets[b], sizeof(plasma_sockets[b]));
        return -LINUX_EFAULT;
    }
    return 0;
}
'''
    text = rep(text, socketpair_old, socketpair_new)

    # Empty connected-stream reads become EOF after the peer's final reference
    # closes, rather than EAGAIN forever.
    socket_read_old = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *sock = &plasma_sockets[entry->object];
        uint64_t done = 0;
        while (done < length && sock->head != sock->tail) {
            uint8_t byte = sock->data[sock->head];
            sock->head = (sock->head + 1u) % PLASMA_SOCKET_BUFFER;
            if (!user_copy_out(address + done, &byte, 1)) return -LINUX_EFAULT;
            ++done;
        }
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
'''
    socket_read_new = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *sock = &plasma_sockets[entry->object];
        uint64_t done = 0;
        while (done < length && sock->head != sock->tail) {
            uint8_t byte = sock->data[sock->head];
            sock->head = (sock->head + 1u) % PLASMA_SOCKET_BUFFER;
            if (!user_copy_out(address + done, &byte, 1)) return -LINUX_EFAULT;
            ++done;
        }
        if (done != 0) return (int64_t)done;
        if (!sock->listening && sock->peer < 0) return 0; /* stream EOF */
        return -LINUX_EAGAIN;
    }
'''
    text = rep(text, socket_read_old, socket_read_new)

    # POLLHUP is reported regardless of requested event bits on a disconnected
    # stream endpoint. Reads then return the EOF above.
    if "#define POLLHUP" not in text:
        text = rep(text, "#define POLLOUT 0x0004\n",
                   "#define POLLOUT 0x0004\n#define POLLHUP 0x0010\n")
    poll_socket_old = r'''        } else if (entry->type == PLASMA_RT_SOCKET) {
            struct plasma_socket_object *s = &plasma_sockets[entry->object];
            if (s->head != s->tail || (s->listening && s->pending >= 0)) *revents |= POLLIN;
        }
'''
    poll_socket_new = r'''        } else if (entry->type == PLASMA_RT_SOCKET) {
            struct plasma_socket_object *s = &plasma_sockets[entry->object];
            if (s->head != s->tail || (s->listening && s->pending >= 0)) *revents |= POLLIN;
        }
    }
    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *s = &plasma_sockets[entry->object];
        if (!s->listening && s->peer < 0) *revents |= POLLHUP;
'''
    text = rep(text, poll_socket_old, poll_socket_new)

    # Real SOL_SOCKET getsockopt.  In particular SO_PEERCRED returns the full
    # 12-byte Linux struct ucred and updates socklen_t, exactly what libdbus
    # validates before accepting EXTERNAL authentication.
    dispatch_old = r'''    case SYS_SHUTDOWN: return 0;
    case SYS_SETSOCKOPT: return 0;
    case SYS_GETSOCKOPT:
        if (a4 != 0 && a5 != 0) { uint32_t len = 0; if (!user_copy_in(&len, a5, sizeof(len))) return -LINUX_EFAULT;
            if (len >= sizeof(uint32_t)) { uint32_t zero = 0; if (!user_copy_out(a4, &zero, sizeof(zero))) return -LINUX_EFAULT; } }
        return 0;
'''
    dispatch_new = r'''    case SYS_SHUTDOWN: return 0;
    case SYS_SETSOCKOPT: return 0;
    case SYS_GETSOCKOPT: return plasma_socket_getsockopt((int)a1, (int)a2, (int)a3, a4, a5);
'''
    text = rep(text, dispatch_old, dispatch_new)

    # Define credentials, getsockopt and lifetime logic after the scheduler and
    # pthread structures exist.  This is before process-exit so exit can release
    # all socket references owned solely by the dying process.
    exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    helpers = r'''static void plasma_current_ucred(struct plasma_ucred *out) {
    if (out == 0) return;
    out->pid = PLASMA_INIT_PID;
    out->uid = 0;
    out->gid = 0;

    if (!plasma_scheduler_ready) return;
    struct plasma_process *process = plasma_current_process();
    if (process == 0) return;
    const int identity_pid = process->tgid > 0 ? process->tgid : process->pid;
    if (identity_pid > 0) out->pid = identity_pid;
}

static int64_t plasma_copy_sockopt(uint64_t value_address,
                                   uint64_t length_address,
                                   const void *value,
                                   uint32_t value_length) {
    if (length_address == 0) return -LINUX_EFAULT;
    uint32_t supplied = 0;
    if (!user_copy_in(&supplied, length_address, sizeof(supplied))) return -LINUX_EFAULT;
    const uint32_t copy = supplied < value_length ? supplied : value_length;
    if (copy != 0) {
        if (value_address == 0 || !user_copy_out(value_address, value, copy))
            return -LINUX_EFAULT;
    }
    if (!user_copy_out(length_address, &value_length, sizeof(value_length)))
        return -LINUX_EFAULT;
    return 0;
}

static int64_t plasma_socket_getsockopt(int fd, int level, int option,
                                        uint64_t value_address,
                                        uint64_t length_address) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_ENOTSOCK;
    if (entry->object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[entry->object].used)
        return -LINUX_ENOTSOCK;
    if (level != PLASMA_SOL_SOCKET) return -LINUX_ENOPROTOOPT;

    struct plasma_socket_object *socket = &plasma_sockets[entry->object];
    if (option == PLASMA_SO_PEERCRED) {
        if (!socket->peer_cred_valid) return -LINUX_ENOTCONN;
        const int64_t rc = plasma_copy_sockopt(value_address, length_address,
                                               &socket->peer_cred,
                                               sizeof(socket->peer_cred));
        if (rc == 0) {
            serial_write("[linux:unix] SO_PEERCRED peer pid=");
            serial_u64((uint64_t)(uint32_t)socket->peer_cred.pid);
            serial_write(" uid=");
            serial_u64(socket->peer_cred.uid);
            serial_write(" gid=");
            serial_u64(socket->peer_cred.gid);
            serial_write("\n");
        }
        return rc;
    }

    int32_t result = 0;
    if (option == PLASMA_SO_TYPE) result = PLASMA_SOCK_STREAM;
    else if (option == PLASMA_SO_ERROR) result = 0;
    else if (option == PLASMA_SO_ACCEPTCONN) result = socket->listening ? 1 : 0;
    else return -LINUX_ENOPROTOOPT;
    return plasma_copy_sockopt(value_address, length_address, &result, sizeof(result));
}

static bool plasma_fd_table_has_socket(const struct plasma_runtime_fd *table,
                                       size_t count, uint16_t object) {
    if (table == 0) return false;
    for (size_t i = 0; i < count; ++i)
        if (table[i].used && table[i].type == PLASMA_RT_SOCKET &&
            table[i].object == object)
            return true;
    return false;
}

static bool plasma_socket_is_pending(uint16_t object) {
    for (uint16_t i = 0; i < PLASMA_SOCKET_OBJECTS; ++i)
        if (plasma_sockets[i].used && plasma_sockets[i].listening &&
            plasma_sockets[i].pending == (int)object)
            return true;
    return false;
}

static bool plasma_socket_has_live_reference(uint16_t object) {
    if (object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[object].used) return false;
    if (plasma_socket_is_pending(object)) return true; /* listen backlog owns it */

    struct plasma_process *current = plasma_current_process();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;

        /* An in-progress blocking read/poll owns the open file even if another
         * CLONE_FILES thread closes the numeric descriptor concurrently. */
        if ((process->state == PLASMA_PROC_BLOCKED_IO &&
             process->blocked_socket_object == object) ||
            (process->state == PLASMA_PROC_BLOCKED_POLL &&
             process->blocked_poll_socket_object == object))
            return true;

        if (process == current) {
            if (plasma_fd_table_has_socket(plasma_runtime_fds,
                    PLASMA_RUNTIME_FD_COUNT, object) ||
                plasma_fd_table_has_socket(plasma_low_runtime_fds, 10u, object))
                return true;
            continue;
        }

        struct plasma_process *owner = plasma_thread_group_leader(process);
        if (owner == 0) owner = process;
        if (plasma_fd_table_has_socket(owner->runtime_fds,
                PLASMA_RUNTIME_FD_COUNT, object) ||
            plasma_fd_table_has_socket(owner->low_runtime_fds, 10u, object))
            return true;
    }
    return false;
}

static void plasma_socket_peer_closed(uint16_t object) {
    if (object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[object].used) return;

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *waiter = &plasma_processes[i];
        if (waiter->state == PLASMA_PROC_BLOCKED_IO &&
            waiter->blocked_socket_object == object) {
            waiter->blocked_pipe_object = 0;
            waiter->blocked_socket_object = 0;
            waiter->blocked_io_address = 0;
            waiter->blocked_io_length = 0;
            waiter->pending_result = 0; /* read(2) EOF */
            waiter->state = PLASMA_PROC_RUNNABLE;
        }

        if (waiter->state == PLASMA_PROC_BLOCKED_POLL &&
            waiter->blocked_poll_socket_object == object &&
            waiter->blocked_poll_address != 0) {
            struct shell_image *saved_image = plasma_active_image;
            struct plasma_process *owner = plasma_thread_group_leader(waiter);
            if (owner == 0) owner = waiter;
            plasma_active_image = &owner->image;

            struct linux_pollfd pfd;
            bool ok = user_copy_in(&pfd, waiter->blocked_poll_address, sizeof(pfd));
            if (ok) {
                pfd.revents = POLLHUP;
                ok = user_copy_out(waiter->blocked_poll_address, &pfd, sizeof(pfd));
            }
            plasma_active_image = saved_image;

            waiter->blocked_poll_socket_object = 0;
            waiter->blocked_poll_address = 0;
            waiter->pending_result = ok ? 1 : -LINUX_EFAULT;
            waiter->state = PLASMA_PROC_RUNNABLE;
        }
    }
}

static void plasma_reclaim_socket_if_unused(uint16_t object) {
    if (object >= PLASMA_SOCKET_OBJECTS || !plasma_sockets[object].used) return;
    if (plasma_socket_has_live_reference(object)) return;

    struct plasma_socket_object *socket = &plasma_sockets[object];
    const int peer = socket->peer;
    socket->peer = -1;

    if (peer >= 0 && peer < PLASMA_SOCKET_OBJECTS && plasma_sockets[peer].used &&
        plasma_sockets[peer].peer == (int)object) {
        plasma_sockets[peer].peer = -1;
        plasma_socket_peer_closed((uint16_t)peer);
    }

    /* If a listening endpoint dies with an unaccepted connection, the backlog
     * reference dies with it and the pending endpoint must be disconnected too. */
    const int pending = socket->pending;
    socket->pending = -1;
    if (pending >= 0 && pending < PLASMA_SOCKET_OBJECTS && plasma_sockets[pending].used) {
        const int pending_peer = plasma_sockets[pending].peer;
        plasma_sockets[pending].peer = -1;
        if (pending_peer >= 0 && pending_peer < PLASMA_SOCKET_OBJECTS &&
            plasma_sockets[pending_peer].used) {
            plasma_sockets[pending_peer].peer = -1;
            plasma_socket_peer_closed((uint16_t)pending_peer);
        }
        bytes_zero(&plasma_sockets[pending], sizeof(plasma_sockets[pending]));
    }

    bytes_zero(socket, sizeof(*socket));
}

static void plasma_reclaim_dead_sockets(void) {
    for (uint16_t object = 0; object < PLASMA_SOCKET_OBJECTS; ++object)
        if (plasma_sockets[object].used)
            plasma_reclaim_socket_if_unused(object);
}

static void plasma_close_cloexec_socket_fds(void) {
    for (int fd = 0; fd < 10; ++fd) {
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && entry->type == PLASMA_RT_SOCKET &&
            (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
    for (int i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        const int fd = PLASMA_RUNTIME_FD_FIRST + i;
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 && entry->type == PLASMA_RT_SOCKET &&
            (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
}

'''
    text = rep(text, exit_anchor, helpers + exit_anchor)

    # eventfd is already applied at this point; extend its close path rather than
    # changing the ordering assumptions of that finalizer.
    close_old = r'''static int64_t plasma_close_runtime(int fd) {
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
    const bool was_socket = entry->type == PLASMA_RT_SOCKET &&
                            entry->object < PLASMA_SOCKET_OBJECTS;
    const uint16_t pipe_object = entry->object;
    const uint16_t eventfd_object = entry->object;
    const uint16_t socket_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    if (was_pipe) plasma_reclaim_pipe_if_unused(pipe_object);
    if (was_eventfd) plasma_reclaim_eventfd_if_unused(eventfd_object);
    if (was_socket) plasma_reclaim_socket_if_unused(socket_object);
    return 0;
}
'''
    text = rep(text, close_old, close_new)

    # Apply socket close-on-exec after the new image is known-good, alongside the
    # existing pipe and eventfd close-on-exec passes.
    text = rep(
        text,
        "    plasma_close_cloexec_pipe_fds();\n"
        "    plasma_close_cloexec_eventfd_fds();\n",
        "    plasma_close_cloexec_pipe_fds();\n"
        "    plasma_close_cloexec_eventfd_fds();\n"
        "    plasma_close_cloexec_socket_fds();\n",
    )

    # A process has stopped owning descriptors once it is a zombie; reclaim any
    # socket endpoint whose last reference disappeared and signal EOF/HUP to its
    # peer before waking wait4 parents.
    text = rep(
        text,
        "    plasma_reclaim_dead_pipes();\n"
        "    plasma_reclaim_dead_eventfds();\n",
        "    plasma_reclaim_dead_pipes();\n"
        "    plasma_reclaim_dead_eventfds();\n"
        "    plasma_reclaim_dead_sockets();\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized AF_UNIX SO_PEERCRED + endpoint lifetime semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
