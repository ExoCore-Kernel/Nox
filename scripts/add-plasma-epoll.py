#!/usr/bin/env python3
"""Add a small level-triggered epoll implementation for Plasma/Xorg bring-up.

Xorg's ospoll backend creates epoll very early.  Twilight already has readiness
logic for its cooperative runtime pipes and AF_UNIX sockets, so epoll can reuse
that instead of fabricating success.  This intentionally implements the small
subset needed for bring-up: create1, ctl ADD/MOD/DEL, wait, and pwait.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected epoll source fragment not found: {old[:160]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "#define SYS_GETDENTS64     217ull\n",
        "#define SYS_GETDENTS64     217ull\n"
        "#define SYS_EPOLL_WAIT     232ull\n"
        "#define SYS_EPOLL_CTL      233ull\n",
    )
    text = rep(
        text,
        "#define SYS_PSELECT6       270ull\n#define SYS_PPOLL          271ull\n",
        "#define SYS_PSELECT6       270ull\n#define SYS_PPOLL          271ull\n"
        "#define SYS_EPOLL_PWAIT    281ull\n",
    )
    text = rep(
        text,
        "#define SYS_ACCEPT4        288ull\n#define SYS_DUP3           292ull\n",
        "#define SYS_ACCEPT4        288ull\n#define SYS_EPOLL_CREATE1  291ull\n"
        "#define SYS_DUP3           292ull\n",
    )

    text = rep(
        text,
        "#define PLASMA_RT_SOCKET 5u\n",
        "#define PLASMA_RT_SOCKET 5u\n#define PLASMA_RT_EPOLL  6u\n",
    )

    state_anchor = "struct __attribute__((packed)) plasma_sockaddr_un {\n"
    state = r'''#define PLASMA_EPOLL_OBJECTS 8
#define PLASMA_EPOLL_WATCHES 64
#define PLASMA_EPOLL_CLOEXEC 02000000u
#define PLASMA_EPOLL_CTL_ADD 1
#define PLASMA_EPOLL_CTL_DEL 2
#define PLASMA_EPOLL_CTL_MOD 3
#define PLASMA_EPOLLIN  0x00000001u
#define PLASMA_EPOLLOUT 0x00000004u
#define PLASMA_EPOLLERR 0x00000008u
#define PLASMA_EPOLLHUP 0x00000010u

struct __attribute__((packed)) plasma_epoll_event {
    uint32_t events;
    uint64_t data;
};

struct plasma_epoll_watch {
    bool used;
    int fd;
    uint32_t events;
    uint64_t data;
};

struct plasma_epoll_object {
    bool used;
    struct plasma_epoll_watch watches[PLASMA_EPOLL_WATCHES];
};

static struct plasma_epoll_object plasma_epolls[PLASMA_EPOLL_OBJECTS];

'''
    text = rep(text, state_anchor, state + state_anchor)

    text = rep(
        text,
        "    bytes_zero(plasma_sockets, sizeof(plasma_sockets));\n",
        "    bytes_zero(plasma_sockets, sizeof(plasma_sockets));\n"
        "    bytes_zero(plasma_epolls, sizeof(plasma_epolls));\n",
    )

    poll_anchor = r'''static int64_t plasma_runtime_poll_one(int fd, int16_t events, int16_t *revents) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || revents == 0) return -LINUX_EBADF;
    *revents = 0;
    if ((events & POLLOUT) != 0 && entry->type != PLASMA_RT_DIR) *revents |= POLLOUT;
    if ((events & POLLIN) != 0) {
        if (entry->type == PLASMA_RT_FILE) *revents |= POLLIN;
        else if (entry->type == PLASMA_RT_PIPE_R) {
            struct plasma_pipe_object *p = &plasma_pipes[entry->object];
            if (p->head != p->tail) *revents |= POLLIN;
        } else if (entry->type == PLASMA_RT_SOCKET) {
            struct plasma_socket_object *s = &plasma_sockets[entry->object];
            if (s->head != s->tail || (s->listening && s->pending >= 0)) *revents |= POLLIN;
        }
    }
    return 0;
}
'''

    epoll_code = poll_anchor + r'''

static int plasma_alloc_epoll_object(void) {
    plasma_runtime_init();
    for (int i = 0; i < PLASMA_EPOLL_OBJECTS; ++i) {
        if (plasma_epolls[i].used) continue;
        bytes_zero(&plasma_epolls[i], sizeof(plasma_epolls[i]));
        plasma_epolls[i].used = true;
        return i;
    }
    return -1;
}

static struct plasma_epoll_object *plasma_epoll_for_fd(int epfd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(epfd);
    if (entry == 0 || entry->type != PLASMA_RT_EPOLL ||
        entry->object >= PLASMA_EPOLL_OBJECTS || !plasma_epolls[entry->object].used)
        return 0;
    return &plasma_epolls[entry->object];
}

static int64_t plasma_epoll_create1(uint32_t flags) {
    if ((flags & ~PLASMA_EPOLL_CLOEXEC) != 0) return -LINUX_EINVAL;
    int object = plasma_alloc_epoll_object();
    if (object < 0) return -LINUX_ENOMEM;
    int fd = plasma_alloc_runtime_fd(PLASMA_RT_EPOLL, (uint16_t)object, flags);
    if (fd < 0) {
        bytes_zero(&plasma_epolls[object], sizeof(plasma_epolls[object]));
        return fd;
    }
    return fd;
}

static int plasma_epoll_find_watch(struct plasma_epoll_object *ep, int fd) {
    if (ep == 0) return -1;
    for (int i = 0; i < PLASMA_EPOLL_WATCHES; ++i)
        if (ep->watches[i].used && ep->watches[i].fd == fd) return i;
    return -1;
}

static bool plasma_epoll_watchable_fd(int fd) {
    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) return runtime->type != PLASMA_RT_EPOLL;
    return fd_is_tty(fd);
}

static int64_t plasma_epoll_ctl(int epfd, int operation, int fd, uint64_t event_address) {
    struct plasma_epoll_object *ep = plasma_epoll_for_fd(epfd);
    if (ep == 0) return -LINUX_EBADF;
    if (fd == epfd || !plasma_epoll_watchable_fd(fd)) return -LINUX_EBADF;

    int index = plasma_epoll_find_watch(ep, fd);
    if (operation == PLASMA_EPOLL_CTL_DEL) {
        if (index < 0) return -LINUX_ENOENT;
        bytes_zero(&ep->watches[index], sizeof(ep->watches[index]));
        return 0;
    }

    struct plasma_epoll_event event;
    if (event_address == 0 || !user_copy_in(&event, event_address, sizeof(event)))
        return -LINUX_EFAULT;

    if (operation == PLASMA_EPOLL_CTL_ADD) {
        if (index >= 0) return -LINUX_EEXIST;
        for (int i = 0; i < PLASMA_EPOLL_WATCHES; ++i) {
            if (ep->watches[i].used) continue;
            ep->watches[i].used = true;
            ep->watches[i].fd = fd;
            ep->watches[i].events = event.events;
            ep->watches[i].data = event.data;
            return 0;
        }
        return -LINUX_ENOMEM;
    }

    if (operation == PLASMA_EPOLL_CTL_MOD) {
        if (index < 0) return -LINUX_ENOENT;
        ep->watches[index].events = event.events;
        ep->watches[index].data = event.data;
        return 0;
    }
    return -LINUX_EINVAL;
}

static int64_t plasma_epoll_probe_fd(int fd, uint32_t events, uint32_t *ready_out) {
    if (ready_out == 0) return -LINUX_EFAULT;
    *ready_out = 0;

    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) {
        if (runtime->type == PLASMA_RT_EPOLL) return 0;
        int16_t wanted = 0, ready = 0;
        if ((events & PLASMA_EPOLLIN) != 0) wanted |= POLLIN;
        if ((events & PLASMA_EPOLLOUT) != 0) wanted |= POLLOUT;
        int64_t rc = plasma_runtime_poll_one(fd, wanted, &ready);
        if (rc < 0) return rc;
        if ((ready & POLLIN) != 0) *ready_out |= PLASMA_EPOLLIN;
        if ((ready & POLLOUT) != 0) *ready_out |= PLASMA_EPOLLOUT;
        return 0;
    }

    if (fd_is_tty(fd)) {
        if ((events & PLASMA_EPOLLIN) != 0 && tty_input_available())
            *ready_out |= PLASMA_EPOLLIN;
        if ((events & PLASMA_EPOLLOUT) != 0)
            *ready_out |= PLASMA_EPOLLOUT;
        return 0;
    }
    return -LINUX_EBADF;
}

static int64_t plasma_epoll_wait(int epfd, uint64_t events_address,
                                 int maxevents, int timeout) {
    (void)timeout;
    struct plasma_epoll_object *ep = plasma_epoll_for_fd(epfd);
    if (ep == 0) return -LINUX_EBADF;
    if (maxevents <= 0) return -LINUX_EINVAL;
    if (maxevents > PLASMA_EPOLL_WATCHES) maxevents = PLASMA_EPOLL_WATCHES;
    if (!user_range(events_address,
                    (uint64_t)maxevents * sizeof(struct plasma_epoll_event), true))
        return -LINUX_EFAULT;

    int emitted = 0;
    for (int i = 0; i < PLASMA_EPOLL_WATCHES && emitted < maxevents; ++i) {
        struct plasma_epoll_watch *watch = &ep->watches[i];
        if (!watch->used) continue;
        uint32_t ready = 0;
        int64_t rc = plasma_epoll_probe_fd(watch->fd, watch->events, &ready);
        if (rc == -LINUX_EBADF) {
            ready = PLASMA_EPOLLERR | PLASMA_EPOLLHUP;
        } else if (rc < 0) {
            return rc;
        }
        ready &= watch->events | PLASMA_EPOLLERR | PLASMA_EPOLLHUP;
        if (ready == 0) continue;
        struct plasma_epoll_event event = { .events = ready, .data = watch->data };
        if (!user_copy_out(events_address +
                           (uint64_t)emitted * sizeof(struct plasma_epoll_event),
                           &event, sizeof(event)))
            return -LINUX_EFAULT;
        ++emitted;
    }

    /* With the current cooperative scheduler, returning 0 for a blocking wait
     * yields at the syscall boundary.  Xorg will re-enter epoll_wait after
     * other runnable processes have had a chance to produce input. */
    return emitted;
}
'''
    text = rep(text, poll_anchor, epoll_code)

    dispatch_anchor = "    case SYS_SOCKET: return plasma_socket_create((int)a1, (int)a2, (int)a3);\n"
    dispatch = r'''    case SYS_EPOLL_CREATE1: return plasma_epoll_create1((uint32_t)a1);
    case SYS_EPOLL_CTL: return plasma_epoll_ctl((int)a1, (int)a2, (int)a3, a4);
    case SYS_EPOLL_WAIT: return plasma_epoll_wait((int)a1, a2, (int)a3, (int)a4);
    case SYS_EPOLL_PWAIT: return plasma_epoll_wait((int)a1, a2, (int)a3, (int)a4);
'''
    text = rep(text, dispatch_anchor, dispatch + dispatch_anchor)

    path.write_text(text, encoding="utf-8")
    print(f"Added Plasma epoll_create1/ctl/wait ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
