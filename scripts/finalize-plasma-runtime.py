#!/usr/bin/env python3
"""Finalize runtime IPC declarations, writev, and process FD inheritance."""
from __future__ import annotations
import pathlib, re, sys

def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected runtime finalizer fragment not found: {old[:150]!r}")
    return text.replace(old,new,count)

def main() -> int:
    if len(sys.argv)!=2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C",file=sys.stderr); return 2
    p=pathlib.Path(sys.argv[1]); text=p.read_text(encoding="utf-8")

    # Do not hard-code the runtime FD base here.  Earlier bring-up transforms
    # may deliberately move it (for example Xtrans needs early listener FDs in
    # the traditional low range).  Locate the generated definition and insert
    # declarations immediately before whatever value is currently selected.
    marker=None
    for line in text.splitlines(keepends=True):
        if line.startswith("#define PLASMA_RUNTIME_FD_FIRST "):
            marker=line
            break
    if marker is None:
        raise RuntimeError("PLASMA_RUNTIME_FD_FIRST definition not found")

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

    # Qt/KDE plus Mesa's software stack can map well over 256 MiB while the
    # first Plasma session is resolving its dependency closure (LLVM alone is
    # large).  The 65,536-page bring-up ceiling was reached in a real boot, so
    # allow 131,072 tracked pages = 512 MiB per process.  This is metadata
    # capacity; physical RAM is still allocated only for pages actually mapped.
    page_pattern = r"(?m)^#define SHELL_MAX_PAGES\s+\d+u$"
    if len(re.findall(page_pattern, text)) != 1:
        raise RuntimeError("expected exactly one SHELL_MAX_PAGES definition")
    text = re.sub(page_pattern, "#define SHELL_MAX_PAGES     131072u", text, count=1)

    # Do not let another page-budget exhaustion look like mysterious linker
    # breakage.  Emit one explicit diagnostic from the common page allocator.
    text=rep(text,
        "    if (image.page_count >= SHELL_MAX_PAGES) return 0;\n",
        "    if (image.page_count >= SHELL_MAX_PAGES) {\n"
        "        serial_write(\"[linux:vm] SHELL_MAX_PAGES exhausted: pages=\");\n"
        "        serial_u64((uint64_t)image.page_count);\n"
        "        serial_write(\" limit=\");\n"
        "        serial_u64((uint64_t)SHELL_MAX_PAGES);\n"
        "        serial_write(\"\\n\");\n"
        "        return 0;\n"
        "    }\n")

    text=rep(text,"#define PLASMA_MAX_PROCESSES 12u\n","#define PLASMA_MAX_PROCESSES 24u\n")

    p.write_text(text,encoding="utf-8"); print(f"Finalized Plasma runtime IPC + per-process FDs + 512 MiB page budget: {p}"); return 0
if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e: print(f"ERROR: {e}",file=sys.stderr); raise SystemExit(1)
