#!/usr/bin/env python3
"""Finalize runtime IPC declarations, writev, and process FD inheritance."""
from __future__ import annotations
import pathlib, sys

def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected runtime finalizer fragment not found: {old[:150]!r}")
    return text.replace(old,new,count)

def main() -> int:
    if len(sys.argv)!=2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C",file=sys.stderr); return 2
    p=pathlib.Path(sys.argv[1]); text=p.read_text(encoding="utf-8")

    marker="#define PLASMA_RUNTIME_FD_FIRST 64\n"
    decls=(
        "static size_t string_length(const char *text);\n"
        "static bool string_equal(const char *a, const char *b);\n"
        "static void bytes_zero(void *pointer, size_t size);\n"
        "static void bytes_copy(void *destination, const void *source, size_t size);\n"
        "static bool user_range(uint64_t address, uint64_t length, bool writable);\n"
        "static bool user_copy_out(uint64_t address, const void *source, uint64_t length);\n"
        "static bool user_copy_in(void *destination, uint64_t address, uint64_t length);\n"
        "static bool user_store_u32(uint64_t address, uint32_t value);\n"
        "static bool user_store_u64(uint64_t address, uint64_t value);\n"
        "static int64_t fill_stat(uint64_t address, uint32_t mode);\n\n")
    text=rep(text,marker,decls+marker)

    # Runtime errno set needs ENODEV for fbdev but do not duplicate it if another
    # transform already supplied one.
    while text.count("#define LINUX_ENODEV     19\n") > 1:
        text=text.replace("#define LINUX_ENODEV     19\n","",1)

    # Preserve runtime descriptors as per-process state and inherit them at fork.
    text=rep(text,
        "    struct rootfs_open_file files[ROOTFS_FD_COUNT];\n",
        "    struct rootfs_open_file files[ROOTFS_FD_COUNT];\n"
        "    struct plasma_runtime_fd runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n")
    text=rep(text,
        "    bytes_copy(process->files, rootfs_open_files, sizeof(rootfs_open_files));\n",
        "    bytes_copy(process->files, rootfs_open_files, sizeof(rootfs_open_files));\n"
        "    bytes_copy(process->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n")
    text=rep(text,
        "    bytes_copy(rootfs_open_files, process->files, sizeof(rootfs_open_files));\n",
        "    bytes_copy(rootfs_open_files, process->files, sizeof(rootfs_open_files));\n"
        "    bytes_copy(plasma_runtime_fds, process->runtime_fds, sizeof(plasma_runtime_fds));\n")
    text=rep(text,
        "    bytes_copy(child->files, rootfs_open_files, sizeof(rootfs_open_files));\n",
        "    bytes_copy(child->files, rootfs_open_files, sizeof(rootfs_open_files));\n"
        "    bytes_copy(child->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n")

    # writev is common for X11 and D-Bus stream traffic.
    old='''    case SYS_WRITEV: {
        if (!fd_is_tty((int)a1)) return -LINUX_EBADF;
        if (a3 > 64 || !user_range(a2, a3 * sizeof(struct linux_iovec), false))
            return -LINUX_EFAULT;
        int64_t total = 0;
        for (uint64_t i = 0; i < a3; ++i) {
            struct linux_iovec iov;
            if (!user_copy_in(&iov, a2 + i * sizeof(iov), sizeof(iov))) return -LINUX_EFAULT;
            const int64_t rc = sys_write_tty((int)a1, iov.base, iov.len);
            if (rc < 0) return rc;
            total += rc;
        }
        return total;
    }
'''
    new='''    case SYS_WRITEV: {
        if (a3 > 64 || !user_range(a2, a3 * sizeof(struct linux_iovec), false))
            return -LINUX_EFAULT;
        const bool runtime = plasma_runtime_fd((int)a1) != 0;
        if (!runtime && !fd_is_tty((int)a1)) return -LINUX_EBADF;
        int64_t total = 0;
        for (uint64_t i = 0; i < a3; ++i) {
            struct linux_iovec iov;
            if (!user_copy_in(&iov, a2 + i * sizeof(iov), sizeof(iov))) return -LINUX_EFAULT;
            const int64_t rc = runtime ? plasma_runtime_write((int)a1, iov.base, iov.len)
                                       : sys_write_tty((int)a1, iov.base, iov.len);
            if (rc < 0) return total != 0 ? total : rc;
            total += rc;
        }
        return total;
    }
'''
    text=rep(text,old,new)

    # Increase resident-page and process metadata ceilings for Qt/KDE binaries.
    text=rep(text,"#define SHELL_MAX_PAGES     768u\n","#define SHELL_MAX_PAGES     16384u\n")
    text=rep(text,"#define PLASMA_MAX_PROCESSES 12u\n","#define PLASMA_MAX_PROCESSES 24u\n")

    p.write_text(text,encoding="utf-8"); print(f"Finalized Plasma runtime IPC + per-process FDs: {p}"); return 0
if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e: print(f"ERROR: {e}",file=sys.stderr); raise SystemExit(1)
