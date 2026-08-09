#!/usr/bin/env python3
"""Finish the small Linux filesystem/process ABI reached by KDE Plasma startup.

After the X11 handshake became fully bidirectional, Plasma reached real KDE
helpers (kapplymousetheme, ksplashqml, ksycoca).  The next failures are ordinary
Linux ABI gaps rather than graphics problems:

* statx(2) is used heavily by Qt/KDE filesystem discovery and plugin scanning;
* flock/fsync/rename are required by QSaveFile/KConfig/ksycoca;
* statfs/fstatfs are queried while classifying filesystems;
* waitid is used by QProcess for child status probing;
* tkill/tgkill are used by libc/Qt fatal paths.  Returning ENOSYS from tkill
  after a fatal Qt error lets the process fall through to a userspace trap,
  which Twilight's early exception stubs currently escalate to a kernel panic.

This remains a bring-up shim: locks are uncontended, fsync is immediate for the
in-memory runtime VFS, statfs reports a synthetic tmpfs-like filesystem, and
waitid implements the nonblocking child-status form used by QProcess.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected KDE ABI fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # x86_64 Linux syscall numbers reached by the KDE/Qt startup path.
    text = rep(
        text,
        "#define SYS_FCNTL           72ull\n",
        "#define SYS_FCNTL           72ull\n"
        "#define SYS_FLOCK           73ull\n"
        "#define SYS_FSYNC           74ull\n"
        "#define SYS_FDATASYNC       75ull\n",
    )
    text = rep(
        text,
        "#define SYS_CHDIR           80ull\n",
        "#define SYS_CHDIR           80ull\n"
        "#define SYS_RENAME          82ull\n",
    )
    text = rep(
        text,
        "#define SYS_SIGALTSTACK    131ull\n",
        "#define SYS_SIGALTSTACK    131ull\n"
        "#define SYS_STATFS         137ull\n"
        "#define SYS_FSTATFS        138ull\n",
    )
    text = rep(
        text,
        "#define SYS_GETTID         186ull\n",
        "#define SYS_GETTID         186ull\n"
        "#define SYS_TKILL          200ull\n",
    )
    text = rep(
        text,
        "#define SYS_TGKILL         234ull\n",
        "#define SYS_TGKILL         234ull\n"
        "#define SYS_WAITID         247ull\n",
    )
    text = rep(
        text,
        "#define SYS_NEWFSTATAT     262ull\n",
        "#define SYS_NEWFSTATAT     262ull\n"
        "#define SYS_RENAMEAT       264ull\n",
    )
    text = rep(
        text,
        "#define SYS_GETRANDOM      318ull\n",
        "#define SYS_RENAMEAT2      316ull\n"
        "#define SYS_GETRANDOM      318ull\n",
    )
    text = rep(
        text,
        "#define SYS_RSEQ           334ull\n",
        "#define SYS_STATX          332ull\n"
        "#define SYS_RSEQ           334ull\n",
    )
    text = rep(
        text,
        "#define LINUX_EPERM       1\n",
        "#define LINUX_EPERM       1\n"
        "#define LINUX_ESRCH       3\n",
    )

    helper_anchor = "static int64_t shell_dispatch(uint64_t number,\n"
    helpers = r'''#define PLASMA_AT_EMPTY_PATH 0x1000u
#define PLASMA_RENAME_NOREPLACE 1u
#define PLASMA_WNOWAIT 0x01000000u
#define PLASMA_SIGABRT 6
#define PLASMA_SIGKILL 9
#define PLASMA_SIGSEGV 11
#define PLASMA_SIGTERM 15
#define PLASMA_SIGILL 4
#define PLASMA_SIGFPE 8

struct plasma_linux_statx_timestamp {
    int64_t sec;
    uint32_t nsec;
    int32_t reserved;
};

struct plasma_linux_statx {
    uint32_t mask;
    uint32_t blksize;
    uint64_t attributes;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t spare0;
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t attributes_mask;
    struct plasma_linux_statx_timestamp atime;
    struct plasma_linux_statx_timestamp btime;
    struct plasma_linux_statx_timestamp ctime;
    struct plasma_linux_statx_timestamp mtime;
    uint32_t rdev_major;
    uint32_t rdev_minor;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint64_t mnt_id;
    uint32_t dio_mem_align;
    uint32_t dio_offset_align;
    uint64_t spare[12];
};

struct plasma_linux_statfs {
    int64_t type;
    int64_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t bavail;
    uint64_t files;
    uint64_t ffree;
    int32_t fsid[2];
    int64_t namelen;
    int64_t frsize;
    int64_t flags;
    int64_t spare[4];
};

static bool plasma_kde_fd_valid(int fd) {
    if (plasma_runtime_fd(fd) != 0 || rootfs_file_for_fd(fd) != 0) return true;
    if (fd_is_tty(fd) || fd == 4) return true;
#ifdef PLASMA_FB_FD
    if (fd == PLASMA_FB_FD) return true;
#endif
    return false;
}

static bool plasma_kde_mode_size_for_fd(int fd, uint32_t *mode, uint64_t *size) {
    if (mode == 0 || size == 0) return false;
    struct plasma_runtime_fd *runtime = plasma_runtime_fd(fd);
    if (runtime != 0) {
        if (runtime->type == PLASMA_RT_FILE || runtime->type == PLASMA_RT_DIR) {
            const struct plasma_tmp_node *node = &plasma_tmp_nodes[runtime->object];
            *mode = node->mode;
            *size = node->size;
        } else {
            *mode = S_IFCHR | 0666u;
            *size = 0;
        }
        return true;
    }
    struct rootfs_open_file *root = rootfs_file_for_fd(fd);
    if (root != 0) {
        *mode = root->node.mode;
        *size = root->node.size;
        return true;
    }
    if (fd_is_tty(fd) || fd == 4) {
        *mode = S_IFCHR | 0666u;
        *size = 0;
        return true;
    }
    return false;
}

static bool plasma_kde_mode_size_for_path(const char *input,
                                          uint32_t *mode,
                                          uint64_t *size,
                                          uint64_t *ino) {
    if (input == 0 || mode == 0 || size == 0 || ino == 0) return false;
    char resolved[256];
    if (!plasma_resolve_path(input, resolved, sizeof(resolved))) return false;

    if (plasma_runtime_path(resolved)) {
        const int index = plasma_find_tmp(resolved);
        if (index >= 0) {
            *mode = plasma_tmp_nodes[index].mode;
            *size = plasma_tmp_nodes[index].size;
            *ino = 0x100000ull + (uint64_t)index;
            return true;
        }
    }

    if (string_equal(resolved, "/dev/tty") || string_equal(resolved, "/dev/null")) {
        *mode = S_IFCHR | 0666u;
        *size = 0;
        *ino = 3;
        return true;
    }
    if (string_equal(resolved, "/dev/fb0") && plasma_fb_available()) {
        *mode = S_IFCHR | 0666u;
        *size = framebuffer_size();
        *ino = 5;
        return true;
    }

    struct rootfs_node node;
    if (rootfs_available() && rootfs_lookup_follow(resolved, &node)) {
        *mode = node.mode;
        *size = node.size;
        /* Stable identity is more important than uniqueness for this statx
         * bring-up path; rootfs fstat has its stronger identity finalizer. */
        *ino = 2;
        return true;
    }
    return false;
}

static int64_t plasma_kde_fill_statx(uint64_t address,
                                     uint32_t mode,
                                     uint64_t size,
                                     uint64_t ino) {
    struct plasma_linux_statx out;
    bytes_zero(&out, sizeof(out));
    out.mask = 0x000007ffu; /* STATX_BASIC_STATS */
    out.blksize = 4096u;
    out.nlink = (mode & 0170000u) == S_IFDIR ? 2u : 1u;
    out.uid = 0;
    out.gid = 0;
    out.mode = (uint16_t)mode;
    out.ino = ino;
    out.size = size;
    out.blocks = (size + 511u) / 512u;
    out.mnt_id = 1;
    return user_copy_out(address, &out, sizeof(out)) ? 0 : -LINUX_EFAULT;
}

static int64_t plasma_kde_statx(int dirfd,
                                uint64_t path_address,
                                uint32_t flags,
                                uint32_t mask,
                                uint64_t statx_address) {
    (void)mask;
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;

    uint32_t mode = 0;
    uint64_t size = 0;
    uint64_t ino = 1;
    if (path[0] == '\0') {
        if ((flags & PLASMA_AT_EMPTY_PATH) == 0) return -LINUX_ENOENT;
        if (!plasma_kde_mode_size_for_fd(dirfd, &mode, &size)) return -LINUX_EBADF;
        ino = 0x200000ull + (uint64_t)(uint32_t)dirfd;
    } else if (!plasma_kde_mode_size_for_path(path, &mode, &size, &ino)) {
        return -LINUX_ENOENT;
    }
    return plasma_kde_fill_statx(statx_address, mode, size, ino);
}

static int64_t plasma_kde_statfs_out(uint64_t address) {
    struct plasma_linux_statfs out;
    bytes_zero(&out, sizeof(out));
    out.type = 0x01021994ll; /* TMPFS_MAGIC: adequate for the in-memory VFS view. */
    out.bsize = 4096;
    out.blocks = 1024ull * 1024ull;
    out.bfree = out.blocks / 2ull;
    out.bavail = out.bfree;
    out.files = 1024ull * 1024ull;
    out.ffree = out.files / 2ull;
    out.namelen = 255;
    out.frsize = 4096;
    return user_copy_out(address, &out, sizeof(out)) ? 0 : -LINUX_EFAULT;
}

static int64_t plasma_kde_statfs_path(uint64_t path_address, uint64_t out_address) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    uint32_t mode = 0;
    uint64_t size = 0, ino = 0;
    if (!plasma_kde_mode_size_for_path(path, &mode, &size, &ino)) return -LINUX_ENOENT;
    return plasma_kde_statfs_out(out_address);
}

static int64_t plasma_kde_rename(uint64_t old_address,
                                 uint64_t new_address,
                                 uint32_t flags) {
    if ((flags & ~PLASMA_RENAME_NOREPLACE) != 0) return -LINUX_EINVAL;

    char old_path[256], new_path[256], old_resolved[256], new_resolved[256];
    if (!copy_user_string(old_address, old_path, sizeof(old_path)) ||
        !copy_user_string(new_address, new_path, sizeof(new_path)))
        return -LINUX_EFAULT;
    if (!plasma_resolve_path(old_path, old_resolved, sizeof(old_resolved)) ||
        !plasma_resolve_path(new_path, new_resolved, sizeof(new_resolved)))
        return -LINUX_ENOENT;
    if (!plasma_runtime_path(old_resolved) || !plasma_runtime_path(new_resolved))
        return -LINUX_EACCES;

    const int source = plasma_find_tmp(old_resolved);
    if (source < 0) return -LINUX_ENOENT;
    const int target = plasma_find_tmp(new_resolved);
    if (target >= 0 && target != source && (flags & PLASMA_RENAME_NOREPLACE) != 0)
        return -LINUX_EEXIST;
    if (target >= 0 && target != source)
        bytes_zero(&plasma_tmp_nodes[target], sizeof(plasma_tmp_nodes[target]));

    const size_t n = string_length(new_resolved);
    if (n + 1u > sizeof(plasma_tmp_nodes[source].path)) return -LINUX_EINVAL;
    bytes_copy(plasma_tmp_nodes[source].path, new_resolved, n + 1u);
    serial_write("[linux:kde] runtime rename ");
    serial_write(old_resolved);
    serial_write(" -> ");
    serial_write(new_resolved);
    serial_write("\n");
    return 0;
}

static int64_t plasma_kde_waitid(int idtype,
                                 int id,
                                 uint64_t info_address,
                                 uint32_t options,
                                 uint64_t rusage_address) {
    plasma_scheduler_init();
    struct plasma_process *parent = plasma_current_process();
    if (parent == 0) return -LINUX_ECHILD;

    int requested = -1;
    if (idtype == 1) requested = id;       /* P_PID */
    else if (idtype == 0 || idtype == 2) requested = -1; /* P_ALL/P_PGID */
    else return -LINUX_EINVAL;

    bool live = false;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *child = &plasma_processes[i];
        if (!plasma_wait_matches(requested, child, parent->pid)) continue;
        if (child->state != PLASMA_PROC_ZOMBIE) {
            live = true;
            continue;
        }

        if (info_address != 0) {
            if (!user_zero(info_address, 128u) ||
                !user_store_u32(info_address + 0u, (uint32_t)PLASMA_SIGCHLD) ||
                !user_store_u32(info_address + 8u, 1u) || /* CLD_EXITED */
                !user_store_u32(info_address + 16u, (uint32_t)child->pid) ||
                !user_store_u32(info_address + 20u, 0u) ||
                !user_store_u32(info_address + 24u, (uint32_t)child->exit_status))
                return -LINUX_EFAULT;
        }
        if (rusage_address != 0 && !user_zero(rusage_address, 144u)) return -LINUX_EFAULT;

        /* Keep the zombie available for wait4 or a WNOWAIT-style second probe.
         * The existing scheduler owns detached-image cleanup. */
        (void)options;
        return 0;
    }

    if ((options & PLASMA_WNOHANG) != 0) {
        if (info_address != 0 && !user_zero(info_address, 128u)) return -LINUX_EFAULT;
        return 0;
    }
    return live ? -LINUX_EAGAIN : -LINUX_ECHILD;
}

static int64_t plasma_kde_thread_signal(int tid, int signal) {
    struct plasma_process *current = plasma_current_process();
    if (current == 0) return 0;
    if (signal == 0) return tid == current->pid ? 0 : -LINUX_ESRCH;
    if (tid != current->pid) return 0;

    if (signal == PLASMA_SIGABRT || signal == PLASMA_SIGKILL ||
        signal == PLASMA_SIGSEGV || signal == PLASMA_SIGTERM ||
        signal == PLASMA_SIGILL || signal == PLASMA_SIGFPE) {
        serial_write("[linux:process] fatal userspace signal pid=");
        serial_u64((uint64_t)current->pid);
        serial_write(" signal=");
        serial_u64((uint64_t)signal);
        serial_write("\n");
        return plasma_process_exit(128 + signal);
    }
    return 0;
}

'''
    text = rep(text, helper_anchor, helpers + helper_anchor)

    # Filesystem/process syscall cases.  Place them before BRK, after all helper
    # definitions and before the generic userspace memory cases.
    cases = r'''    case SYS_FLOCK:
        return plasma_kde_fd_valid((int)a1) ? 0 : -LINUX_EBADF;
    case SYS_FSYNC:
    case SYS_FDATASYNC:
        return plasma_kde_fd_valid((int)a1) ? 0 : -LINUX_EBADF;
    case SYS_RENAME:
        return plasma_kde_rename(a1, a2, 0);
    case SYS_RENAMEAT:
        return plasma_kde_rename(a2, a4, 0);
    case SYS_RENAMEAT2:
        return plasma_kde_rename(a2, a4, (uint32_t)a5);
    case SYS_STATFS:
        return plasma_kde_statfs_path(a1, a2);
    case SYS_FSTATFS:
        return plasma_kde_fd_valid((int)a1) ? plasma_kde_statfs_out(a2) : -LINUX_EBADF;
    case SYS_STATX:
        return plasma_kde_statx((int)a1, a2, (uint32_t)a3, (uint32_t)a4, a5);
    case SYS_WAITID:
        return plasma_kde_waitid((int)a1, (int)a2, a3, (uint32_t)a4, a5);
'''
    text = rep(text, "    case SYS_BRK: return sys_brk(a1);\n", cases + "    case SYS_BRK: return sys_brk(a1);\n")

    # Turn libc/Qt self-directed fatal signals into a normal process death so a
    # failed KDE helper cannot deliberately trap into Twilight's panic-only IDT.
    text = rep(
        text,
        "    case SYS_KILL:\n"
        "    case SYS_TGKILL: return 0;\n",
        "    case SYS_KILL: return 0;\n"
        "    case SYS_TKILL: return plasma_kde_thread_signal((int)a1, (int)a2);\n"
        "    case SYS_TGKILL: return plasma_kde_thread_signal((int)a2, (int)a3);\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized KDE statx/QSaveFile/waitid/fatal-signal ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
