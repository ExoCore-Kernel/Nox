#!/usr/bin/env python3
"""Add a small writable runtime VFS, pipes, and AF_UNIX sockets.

This is intentionally a bring-up implementation rather than a complete Linux
VFS/network stack.  It supplies the primitives Xorg and D-Bus need first:
/tmp and /run files, pipe2, and local stream socket bind/listen/connect/accept
with in-kernel byte queues.  Descriptor state is per process; runtime objects are
shared by descriptor index and therefore survive fork.
"""

from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected runtime IPC fragment not found: {old[:140]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(text, "#define SYS_GETPID          39ull\n",
               "#define SYS_GETPID          39ull\n"
               "#define SYS_SOCKET          41ull\n#define SYS_CONNECT         42ull\n"
               "#define SYS_ACCEPT          43ull\n#define SYS_SENDTO          44ull\n"
               "#define SYS_RECVFROM        45ull\n#define SYS_SENDMSG         46ull\n"
               "#define SYS_RECVMSG         47ull\n#define SYS_SHUTDOWN        48ull\n"
               "#define SYS_BIND            49ull\n#define SYS_LISTEN          50ull\n"
               "#define SYS_GETSOCKNAME     51ull\n#define SYS_GETPEERNAME     52ull\n"
               "#define SYS_SOCKETPAIR      53ull\n#define SYS_SETSOCKOPT      54ull\n"
               "#define SYS_GETSOCKOPT      55ull\n")
    text = rep(text, "#define SYS_CHDIR           80ull\n",
               "#define SYS_CHDIR           80ull\n#define SYS_MKDIR           83ull\n"
               "#define SYS_RMDIR           84ull\n#define SYS_UNLINK          87ull\n")
    text = rep(text, "#define SYS_OPENAT         257ull\n",
               "#define SYS_OPENAT         257ull\n#define SYS_MKDIRAT        258ull\n"
               "#define SYS_UNLINKAT       263ull\n")
    text = rep(text, "#define SYS_DUP3           292ull\n",
               "#define SYS_ACCEPT4        288ull\n#define SYS_DUP3           292ull\n")

    text = rep(text, "#define LINUX_EEXIST     17\n",
               "#define LINUX_EEXIST     17\n#define LINUX_ENODEV     19\n"
               "#define LINUX_ENOTEMPTY  39\n#define LINUX_EPIPE      32\n"
               "#define LINUX_EAFNOSUPPORT 97\n#define LINUX_EADDRINUSE 98\n"
               "#define LINUX_ECONNREFUSED 111\n")

    # Types must exist before add-plasma-processes.py injects its process struct.
    anchor = "static struct rootfs_open_file rootfs_open_files[ROOTFS_FD_COUNT];\n"
    type_block = r'''
#define PLASMA_RUNTIME_FD_FIRST 64
#define PLASMA_RUNTIME_FD_COUNT 48
#define PLASMA_TMP_NODES 32
#define PLASMA_TMP_DATA 16384
#define PLASMA_PIPE_OBJECTS 16
#define PLASMA_PIPE_BUFFER 8192
#define PLASMA_SOCKET_OBJECTS 24
#define PLASMA_SOCKET_BUFFER 16384

#define PLASMA_RT_NONE 0u
#define PLASMA_RT_FILE 1u
#define PLASMA_RT_DIR  2u
#define PLASMA_RT_PIPE_R 3u
#define PLASMA_RT_PIPE_W 4u
#define PLASMA_RT_SOCKET 5u

struct plasma_runtime_fd {
    bool used;
    uint8_t type;
    uint16_t object;
    uint32_t flags;
    uint64_t offset;
};

'''
    text = rep(text, anchor, type_block + anchor)

    runtime_state = r'''

#define PLASMA_O_CREAT     00000100u
#define PLASMA_O_TRUNC     00001000u
#define PLASMA_O_NONBLOCK  00004000u
#define PLASMA_O_DIRECTORY 00200000u
#define PLASMA_AF_UNIX 1
#define PLASMA_SOCK_STREAM 1
#define PLASMA_SOCK_NONBLOCK 00004000u
#define PLASMA_SOCK_CLOEXEC 02000000u
#define PLASMA_AT_FDCWD (-100)

struct plasma_tmp_node {
    bool used;
    bool directory;
    char path[256];
    uint32_t mode;
    size_t size;
    uint8_t data[PLASMA_TMP_DATA];
};

struct plasma_pipe_object {
    bool used;
    uint8_t data[PLASMA_PIPE_BUFFER];
    size_t head;
    size_t tail;
};

struct plasma_socket_object {
    bool used;
    bool listening;
    int peer;
    int pending;
    char path[108];
    uint8_t data[PLASMA_SOCKET_BUFFER];
    size_t head;
    size_t tail;
};

struct __attribute__((packed)) plasma_sockaddr_un {
    uint16_t family;
    char path[108];
};

struct plasma_msghdr {
    uint64_t name;
    uint32_t namelen;
    uint32_t pad0;
    uint64_t iov;
    uint64_t iovlen;
    uint64_t control;
    uint64_t controllen;
    uint32_t flags;
    uint32_t pad1;
};

static struct plasma_runtime_fd plasma_runtime_fds[PLASMA_RUNTIME_FD_COUNT];
static struct plasma_tmp_node plasma_tmp_nodes[PLASMA_TMP_NODES];
static struct plasma_pipe_object plasma_pipes[PLASMA_PIPE_OBJECTS];
static struct plasma_socket_object plasma_sockets[PLASMA_SOCKET_OBJECTS];
static bool plasma_runtime_initialized;

static void plasma_runtime_init(void) {
    if (plasma_runtime_initialized) return;
    bytes_zero(plasma_runtime_fds, sizeof(plasma_runtime_fds));
    bytes_zero(plasma_tmp_nodes, sizeof(plasma_tmp_nodes));
    bytes_zero(plasma_pipes, sizeof(plasma_pipes));
    bytes_zero(plasma_sockets, sizeof(plasma_sockets));
    const char *dirs[] = { "/tmp", "/tmp/.X11-unix", "/tmp/runtime-root", "/run", "/run/user", "/run/user/0" };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        plasma_tmp_nodes[i].used = true;
        plasma_tmp_nodes[i].directory = true;
        plasma_tmp_nodes[i].mode = S_IFDIR | 0777u;
        size_t n = string_length(dirs[i]);
        if (n >= sizeof(plasma_tmp_nodes[i].path)) n = sizeof(plasma_tmp_nodes[i].path) - 1u;
        bytes_copy(plasma_tmp_nodes[i].path, dirs[i], n);
        plasma_tmp_nodes[i].path[n] = '\0';
    }
    plasma_runtime_initialized = true;
}

static struct plasma_runtime_fd *plasma_runtime_fd(int fd) {
    if (fd < PLASMA_RUNTIME_FD_FIRST || fd >= PLASMA_RUNTIME_FD_FIRST + PLASMA_RUNTIME_FD_COUNT)
        return 0;
    struct plasma_runtime_fd *entry = &plasma_runtime_fds[fd - PLASMA_RUNTIME_FD_FIRST];
    return entry->used ? entry : 0;
}

static int plasma_alloc_runtime_fd(uint8_t type, uint16_t object, uint32_t flags) {
    plasma_runtime_init();
    for (int i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        if (plasma_runtime_fds[i].used) continue;
        plasma_runtime_fds[i].used = true;
        plasma_runtime_fds[i].type = type;
        plasma_runtime_fds[i].object = object;
        plasma_runtime_fds[i].flags = flags;
        plasma_runtime_fds[i].offset = 0;
        return PLASMA_RUNTIME_FD_FIRST + i;
    }
    return -LINUX_EBUSY;
}

static bool plasma_runtime_path(const char *path) {
    return path != 0 &&
           ((path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
             (path[4] == '\0' || path[4] == '/')) ||
            (path[0] == '/' && path[1] == 'r' && path[2] == 'u' && path[3] == 'n' &&
             (path[4] == '\0' || path[4] == '/')));
}

static int plasma_find_tmp(const char *path) {
    plasma_runtime_init();
    for (int i = 0; i < PLASMA_TMP_NODES; ++i)
        if (plasma_tmp_nodes[i].used && string_equal(plasma_tmp_nodes[i].path, path)) return i;
    return -1;
}

static int plasma_create_tmp(const char *path, bool directory, uint32_t mode) {
    if (!plasma_runtime_path(path)) return -LINUX_EACCES;
    int existing = plasma_find_tmp(path);
    if (existing >= 0) return existing;
    for (int i = 0; i < PLASMA_TMP_NODES; ++i) {
        if (plasma_tmp_nodes[i].used) continue;
        size_t n = string_length(path);
        if (n == 0 || n >= sizeof(plasma_tmp_nodes[i].path)) return -LINUX_EINVAL;
        plasma_tmp_nodes[i].used = true;
        plasma_tmp_nodes[i].directory = directory;
        plasma_tmp_nodes[i].mode = (directory ? S_IFDIR : S_IFREG) | (mode & 0777u);
        bytes_copy(plasma_tmp_nodes[i].path, path, n + 1u);
        return i;
    }
    return -LINUX_ENOMEM;
}

static int64_t plasma_tmp_stat(uint64_t address, int index) {
    if (index < 0 || index >= PLASMA_TMP_NODES || !plasma_tmp_nodes[index].used) return -LINUX_ENOENT;
    int64_t rc = fill_stat(address, plasma_tmp_nodes[index].mode);
    if (rc != 0) return rc;
    if (!user_store_u64(address + 48, plasma_tmp_nodes[index].size)) return -LINUX_EFAULT;
    if (!user_store_u64(address + 56, 4096)) return -LINUX_EFAULT;
    return 0;
}

static int64_t plasma_open_runtime(const char *path, uint32_t flags, uint32_t mode) {
    if (!plasma_runtime_path(path)) return -LINUX_ENOENT;
    int index = plasma_find_tmp(path);
    if (index < 0) {
        if ((flags & PLASMA_O_CREAT) == 0) return -LINUX_ENOENT;
        index = plasma_create_tmp(path, false, mode ? mode : 0666u);
        if (index < 0) return index;
    }
    struct plasma_tmp_node *node = &plasma_tmp_nodes[index];
    if ((flags & PLASMA_O_DIRECTORY) != 0 && !node->directory) return -LINUX_ENOTDIR;
    if ((flags & PLASMA_O_TRUNC) != 0 && !node->directory) node->size = 0;
    return plasma_alloc_runtime_fd(node->directory ? PLASMA_RT_DIR : PLASMA_RT_FILE,
                                   (uint16_t)index, flags);
}

static int64_t plasma_runtime_read(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (length == 0) return 0;
    if (!user_range(address, length, true)) return -LINUX_EFAULT;
    if (entry->type == PLASMA_RT_FILE) {
        struct plasma_tmp_node *node = &plasma_tmp_nodes[entry->object];
        if (entry->offset >= node->size) return 0;
        uint64_t remaining = node->size - entry->offset;
        if (length > remaining) length = remaining;
        if (!user_copy_out(address, node->data + entry->offset, length)) return -LINUX_EFAULT;
        entry->offset += length;
        return (int64_t)length;
    }
    if (entry->type == PLASMA_RT_PIPE_R) {
        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];
        uint64_t done = 0;
        while (done < length && pipe->head != pipe->tail) {
            uint8_t byte = pipe->data[pipe->head];
            pipe->head = (pipe->head + 1u) % PLASMA_PIPE_BUFFER;
            if (!user_copy_out(address + done, &byte, 1)) return -LINUX_EFAULT;
            ++done;
        }
        return done != 0 ? (int64_t)done : -LINUX_EAGAIN;
    }
    if (entry->type == PLASMA_RT_SOCKET) {
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
    return -LINUX_EBADF;
}

static int64_t plasma_runtime_write(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (length == 0) return 0;
    if (!user_range(address, length, false)) return -LINUX_EFAULT;
    if (entry->type == PLASMA_RT_FILE) {
        struct plasma_tmp_node *node = &plasma_tmp_nodes[entry->object];
        if (entry->offset >= PLASMA_TMP_DATA) return -LINUX_ENOMEM;
        uint64_t space = PLASMA_TMP_DATA - entry->offset;
        if (length > space) length = space;
        if (!user_copy_in(node->data + entry->offset, address, length)) return -LINUX_EFAULT;
        entry->offset += length;
        if (entry->offset > node->size) node->size = (size_t)entry->offset;
        return (int64_t)length;
    }
    if (entry->type == PLASMA_RT_PIPE_W) {
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
    if (entry->type == PLASMA_RT_SOCKET) {
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
    return -LINUX_EBADF;
}

static int64_t plasma_runtime_lseek(int fd, int64_t offset, int whence) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_FILE) return -LINUX_ESPIPE;
    struct plasma_tmp_node *node = &plasma_tmp_nodes[entry->object];
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)entry->offset :
                   whence == 2 ? (int64_t)node->size : -1;
    if (base < 0) return -LINUX_EINVAL;
    int64_t next = base + offset;
    if (next < 0 || next > PLASMA_TMP_DATA) return -LINUX_EINVAL;
    entry->offset = (uint64_t)next;
    return next;
}

static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    bytes_zero(entry, sizeof(*entry));
    return 0;
}

static int plasma_alloc_pipe_object(void) {
    for (int i = 0; i < PLASMA_PIPE_OBJECTS; ++i) if (!plasma_pipes[i].used) {
        bytes_zero(&plasma_pipes[i], sizeof(plasma_pipes[i]));
        plasma_pipes[i].used = true;
        return i;
    }
    return -1;
}

static int64_t plasma_pipe2(uint64_t pair_address, uint32_t flags) {
    plasma_runtime_init();
    int object = plasma_alloc_pipe_object();
    if (object < 0) return -LINUX_ENOMEM;
    int read_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_R, (uint16_t)object, flags);
    int write_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_W, (uint16_t)object, flags);
    if (read_fd < 0 || write_fd < 0) return -LINUX_EBUSY;
    int32_t pair[2] = { read_fd, write_fd };
    return user_copy_out(pair_address, pair, sizeof(pair)) ? 0 : -LINUX_EFAULT;
}

static int plasma_alloc_socket_object(void) {
    for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i) if (!plasma_sockets[i].used) {
        bytes_zero(&plasma_sockets[i], sizeof(plasma_sockets[i]));
        plasma_sockets[i].used = true;
        plasma_sockets[i].peer = -1;
        plasma_sockets[i].pending = -1;
        return i;
    }
    return -1;
}

static int64_t plasma_socket_create(int domain, int type, int protocol) {
    (void)protocol;
    plasma_runtime_init();
    if (domain != PLASMA_AF_UNIX) return -LINUX_EAFNOSUPPORT;
    if ((type & 0xf) != PLASMA_SOCK_STREAM) return -LINUX_ENOSYS;
    int object = plasma_alloc_socket_object();
    if (object < 0) return -LINUX_ENOMEM;
    return plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)object, (uint32_t)type);
}

static bool plasma_copy_sockaddr(uint64_t address, uint64_t length, struct plasma_sockaddr_un *out) {
    if (out == 0 || length < 2u) return false;
    bytes_zero(out, sizeof(*out));
    uint64_t copy = length < sizeof(*out) ? length : sizeof(*out);
    if (!user_copy_in(out, address, copy)) return false;
    out->path[sizeof(out->path) - 1u] = '\0';
    return true;
}

static int64_t plasma_socket_bind(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_EBADF;
    struct plasma_sockaddr_un un;
    if (!plasma_copy_sockaddr(address, length, &un)) return -LINUX_EFAULT;
    if (un.family != PLASMA_AF_UNIX) return -LINUX_EAFNOSUPPORT;
    for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i)
        if (plasma_sockets[i].used && plasma_sockets[i].path[0] != '\0' &&
            string_equal(plasma_sockets[i].path, un.path)) return -LINUX_EADDRINUSE;
    struct plasma_socket_object *sock = &plasma_sockets[entry->object];
    size_t n = string_length(un.path);
    if (n >= sizeof(sock->path)) return -LINUX_EINVAL;
    bytes_copy(sock->path, un.path, n + 1u);
    return 0;
}

static int64_t plasma_socket_listen(int fd, int backlog) {
    (void)backlog;
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_EBADF;
    plasma_sockets[entry->object].listening = true;
    return 0;
}

static int64_t plasma_socket_connect(int fd, uint64_t address, uint64_t length) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_EBADF;
    struct plasma_sockaddr_un un;
    if (!plasma_copy_sockaddr(address, length, &un)) return -LINUX_EFAULT;
    if (un.family != PLASMA_AF_UNIX) return -LINUX_EAFNOSUPPORT;
    int listener = -1;
    for (int i = 0; i < PLASMA_SOCKET_OBJECTS; ++i)
        if (plasma_sockets[i].used && plasma_sockets[i].listening &&
            string_equal(plasma_sockets[i].path, un.path)) { listener = i; break; }
    if (listener < 0) return -LINUX_ECONNREFUSED;
    if (plasma_sockets[listener].pending >= 0) return -LINUX_EAGAIN;
    int server = plasma_alloc_socket_object();
    if (server < 0) return -LINUX_ENOMEM;
    plasma_sockets[entry->object].peer = server;
    plasma_sockets[server].peer = entry->object;
    plasma_sockets[listener].pending = server;
    return 0;
}

static int64_t plasma_socket_accept(int fd, uint64_t address, uint64_t length_address, uint32_t flags) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_EBADF;
    struct plasma_socket_object *listener = &plasma_sockets[entry->object];
    if (!listener->listening || listener->pending < 0) return -LINUX_EAGAIN;
    int server = listener->pending;
    listener->pending = -1;
    int new_fd = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)server, flags);
    if (new_fd < 0) return new_fd;
    if (address != 0 && length_address != 0) {
        uint32_t supplied = 0;
        if (!user_copy_in(&supplied, length_address, sizeof(supplied))) return -LINUX_EFAULT;
        struct plasma_sockaddr_un un;
        bytes_zero(&un, sizeof(un));
        un.family = PLASMA_AF_UNIX;
        uint32_t actual = sizeof(uint16_t);
        if (!user_copy_out(address, &un, supplied < sizeof(un) ? supplied : sizeof(un)) ||
            !user_copy_out(length_address, &actual, sizeof(actual))) return -LINUX_EFAULT;
    }
    return new_fd;
}

static int64_t plasma_socket_name(int fd, uint64_t address, uint64_t length_address, bool peer) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0 || entry->type != PLASMA_RT_SOCKET) return -LINUX_EBADF;
    struct plasma_socket_object *sock = &plasma_sockets[entry->object];
    if (peer && sock->peer >= 0) sock = &plasma_sockets[sock->peer];
    uint32_t supplied = 0;
    if (!user_copy_in(&supplied, length_address, sizeof(supplied))) return -LINUX_EFAULT;
    struct plasma_sockaddr_un un;
    bytes_zero(&un, sizeof(un));
    un.family = PLASMA_AF_UNIX;
    size_t n = string_length(sock->path);
    if (n >= sizeof(un.path)) n = sizeof(un.path) - 1u;
    bytes_copy(un.path, sock->path, n);
    uint32_t actual = (uint32_t)(sizeof(uint16_t) + n + 1u);
    uint32_t copy = supplied < actual ? supplied : actual;
    if (!user_copy_out(address, &un, copy) || !user_copy_out(length_address, &actual, sizeof(actual)))
        return -LINUX_EFAULT;
    return 0;
}

static int64_t plasma_socketpair(int domain, int type, int protocol, uint64_t pair_address) {
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

static int64_t plasma_sendmsg(int fd, uint64_t msg_address) {
    struct plasma_msghdr msg;
    if (!user_copy_in(&msg, msg_address, sizeof(msg)) || msg.iovlen > 64) return -LINUX_EFAULT;
    int64_t total = 0;
    for (uint64_t i = 0; i < msg.iovlen; ++i) {
        struct linux_iovec iov;
        if (!user_copy_in(&iov, msg.iov + i * sizeof(iov), sizeof(iov))) return -LINUX_EFAULT;
        int64_t rc = plasma_runtime_write(fd, iov.base, iov.len);
        if (rc < 0) return total != 0 ? total : rc;
        total += rc;
    }
    return total;
}

static int64_t plasma_recvmsg(int fd, uint64_t msg_address) {
    struct plasma_msghdr msg;
    if (!user_copy_in(&msg, msg_address, sizeof(msg)) || msg.iovlen > 64) return -LINUX_EFAULT;
    int64_t total = 0;
    for (uint64_t i = 0; i < msg.iovlen; ++i) {
        struct linux_iovec iov;
        if (!user_copy_in(&iov, msg.iov + i * sizeof(iov), sizeof(iov))) return -LINUX_EFAULT;
        int64_t rc = plasma_runtime_read(fd, iov.base, iov.len);
        if (rc < 0) return total != 0 ? total : rc;
        total += rc;
        if ((uint64_t)rc < iov.len) break;
    }
    msg.flags = 0;
    (void)user_copy_out(msg_address, &msg, sizeof(msg));
    return total;
}

static int64_t plasma_runtime_fstat(int fd, uint64_t address) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;
    if (entry->type == PLASMA_RT_FILE || entry->type == PLASMA_RT_DIR)
        return plasma_tmp_stat(address, entry->object);
    return fill_stat(address, S_IFCHR | 0666u);
}

static int64_t plasma_runtime_poll_one(int fd, int16_t events, int16_t *revents) {
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
    text = rep(text, anchor, anchor + runtime_state)

    # Runtime path checks before rootfs fallback.
    text = rep(text,
        "static bool path_is_known(const char *path) {\n",
        "static bool path_is_known(const char *path) {\n"
        "    if (plasma_runtime_path(path) && plasma_find_tmp(path) >= 0) return true;\n")
    text = rep(text,
        "static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {\n"
        "    char path[256];\n"
        "    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;\n",
        "static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {\n"
        "    char path[256];\n"
        "    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;\n"
        "    if (plasma_runtime_path(path)) { int node = plasma_find_tmp(path); if (node >= 0) return plasma_tmp_stat(stat_address, node); }\n")

    # Change open helper signature to expose flags/mode and route runtime paths.
    text = rep(text,
        "static int64_t sys_open_path(uint64_t path_address) {\n",
        "static int64_t sys_open_path(uint64_t path_address, uint32_t flags, uint32_t mode) {\n")
    text = rep(text,
        "    if (string_equal(path, \"/dev/null\")) return 4;\n",
        "    if (string_equal(path, \"/dev/null\")) return 4;\n"
        "    if (plasma_runtime_path(path)) return plasma_open_runtime(path, flags, mode);\n")

    text = rep(text, "    case SYS_OPEN: return sys_open_path(a1);\n",
               "    case SYS_OPEN: return sys_open_path(a1, (uint32_t)a2, (uint32_t)a3);\n")
    text = rep(text, "    case SYS_OPENAT: return sys_open_path(a2);\n",
               "    case SYS_OPENAT: return sys_open_path(a2, (uint32_t)a3, (uint32_t)a4);\n")

    # read/write/fstat/close/lseek/fcntl/poll descriptor dispatch.
    text = rep(text,
        "    case SYS_READ:\n        if ((int)a1 == 4) return 0; /* /dev/null */\n",
        "    case SYS_READ:\n        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_read((int)a1, a2, a3);\n"
        "        if ((int)a1 == 4) return 0; /* /dev/null */\n")
    text = rep(text,
        "    case SYS_WRITE:\n        if ((int)a1 == 4) return (int64_t)a3;\n",
        "    case SYS_WRITE:\n        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_write((int)a1, a2, a3);\n"
        "        if ((int)a1 == 4) return (int64_t)a3;\n")
    text = rep(text,
        "    case SYS_FSTAT: {\n        struct rootfs_open_file *file = rootfs_file_for_fd((int)a1);\n",
        "    case SYS_FSTAT: {\n        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_fstat((int)a1, a2);\n"
        "        struct rootfs_open_file *file = rootfs_file_for_fd((int)a1);\n")
    text = rep(text,
        "    case SYS_CLOSE:\n        if (rootfs_file_for_fd((int)a1) != 0) return rootfs_close_fd((int)a1);\n",
        "    case SYS_CLOSE:\n        if (plasma_runtime_fd((int)a1) != 0) return plasma_close_runtime((int)a1);\n"
        "        if (rootfs_file_for_fd((int)a1) != 0) return rootfs_close_fd((int)a1);\n")
    text = rep(text,
        "    case SYS_LSEEK:\n        if (rootfs_file_for_fd((int)a1) != 0)\n",
        "    case SYS_LSEEK:\n        if (plasma_runtime_fd((int)a1) != 0) return plasma_runtime_lseek((int)a1, (int64_t)a2, (int)a3);\n"
        "        if (rootfs_file_for_fd((int)a1) != 0)\n")

    # mkdir/unlink for runtime paths.
    dispatch_anchor = "    case SYS_GETCWD: {\n"
    mkdir_cases = r'''    case SYS_MKDIR:
    case SYS_MKDIRAT: {
        uint64_t path_address = number == SYS_MKDIR ? a1 : a2;
        uint64_t mode = number == SYS_MKDIR ? a2 : a3;
        char path[256];
        if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
        int existing = plasma_find_tmp(path);
        if (existing >= 0) return -LINUX_EEXIST;
        int node = plasma_create_tmp(path, true, (uint32_t)mode);
        return node < 0 ? node : 0;
    }
    case SYS_UNLINK:
    case SYS_UNLINKAT: {
        uint64_t path_address = number == SYS_UNLINK ? a1 : a2;
        char path[256];
        if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
        int node = plasma_find_tmp(path);
        if (node < 0) return -LINUX_ENOENT;
        bytes_zero(&plasma_tmp_nodes[node], sizeof(plasma_tmp_nodes[node]));
        return 0;
    }
'''
    text = rep(text, dispatch_anchor, mkdir_cases + dispatch_anchor)

    # Pipe/socket calls.
    text = rep(text, "    case SYS_PIPE2:\n        return -LINUX_ENOSYS;\n",
        "    case SYS_PIPE2: return plasma_pipe2(a1, (uint32_t)a2);\n")
    socket_cases = r'''    case SYS_SOCKET: return plasma_socket_create((int)a1, (int)a2, (int)a3);
    case SYS_BIND: return plasma_socket_bind((int)a1, a2, a3);
    case SYS_LISTEN: return plasma_socket_listen((int)a1, (int)a2);
    case SYS_CONNECT: return plasma_socket_connect((int)a1, a2, a3);
    case SYS_ACCEPT: return plasma_socket_accept((int)a1, a2, a3, 0);
    case SYS_ACCEPT4: return plasma_socket_accept((int)a1, a2, a3, (uint32_t)a4);
    case SYS_GETSOCKNAME: return plasma_socket_name((int)a1, a2, a3, false);
    case SYS_GETPEERNAME: return plasma_socket_name((int)a1, a2, a3, true);
    case SYS_SOCKETPAIR: return plasma_socketpair((int)a1, (int)a2, (int)a3, a4);
    case SYS_SENDTO: return plasma_runtime_write((int)a1, a2, a3);
    case SYS_RECVFROM: return plasma_runtime_read((int)a1, a2, a3);
    case SYS_SENDMSG: return plasma_sendmsg((int)a1, a2);
    case SYS_RECVMSG: return plasma_recvmsg((int)a1, a2);
    case SYS_SHUTDOWN: return 0;
    case SYS_SETSOCKOPT: return 0;
    case SYS_GETSOCKOPT:
        if (a4 != 0 && a5 != 0) { uint32_t len = 0; if (!user_copy_in(&len, a5, sizeof(len))) return -LINUX_EFAULT;
            if (len >= sizeof(uint32_t)) { uint32_t zero = 0; if (!user_copy_out(a4, &zero, sizeof(zero))) return -LINUX_EFAULT; } }
        return 0;
'''
    text = rep(text, "    case SYS_BRK: return sys_brk(a1);\n", socket_cases + "    case SYS_BRK: return sys_brk(a1);\n")

    # Runtime poll readiness inside the existing loop.
    old_poll = "            pfd.revents = 0;\n            if (pfd.fd == 0 || pfd.fd == 3) {\n"
    new_poll = "            pfd.revents = 0;\n            if (plasma_runtime_fd(pfd.fd) != 0) {\n                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n                if ((pfd.events & POLLIN) != 0) wants_input = true;\n            } else if (pfd.fd == 0 || pfd.fd == 3) {\n"
    text = rep(text, old_poll, new_poll)

    # Runtime fcntl basics and descriptor duplication by table copy.
    fcntl_anchor = "static int64_t sys_fcntl(int fd, uint64_t command, uint64_t argument) {\n    (void)argument;\n"
    fcntl_runtime = fcntl_anchor + r'''    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
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
    text = rep(text, fcntl_anchor, fcntl_runtime)

    path.write_text(text, encoding="utf-8")
    print(f"Added writable runtime VFS + pipe2 + AF_UNIX stream sockets: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
