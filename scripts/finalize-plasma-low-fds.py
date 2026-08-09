#!/usr/bin/env python3
"""Allow runtime pipe/socket descriptors to replace low stdio descriptors."""
from __future__ import annotations
import pathlib, sys

def rep(text:str,old:str,new:str,count:int=1)->str:
    if text.count(old)<count: raise RuntimeError(f"expected low-fd fragment not found: {old[:150]!r}")
    return text.replace(old,new,count)

def main()->int:
    if len(sys.argv)!=2: print(f"usage: {sys.argv[0]} GENERATED_BASH_C",file=sys.stderr); return 2
    p=pathlib.Path(sys.argv[1]); text=p.read_text(encoding="utf-8")

    text=rep(text,
        "static struct plasma_runtime_fd plasma_runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n",
        "static struct plasma_runtime_fd plasma_runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n"
        "static struct plasma_runtime_fd plasma_low_runtime_fds[10];\n")
    text=rep(text,
        "static struct plasma_runtime_fd *plasma_runtime_fd(int fd) {\n"
        "    if (fd < PLASMA_RUNTIME_FD_FIRST || fd >= PLASMA_RUNTIME_FD_FIRST + PLASMA_RUNTIME_FD_COUNT)\n"
        "        return 0;\n",
        "static struct plasma_runtime_fd *plasma_runtime_fd(int fd) {\n"
        "    if (fd >= 0 && fd < 10 && plasma_low_runtime_fds[fd].used) return &plasma_low_runtime_fds[fd];\n"
        "    if (fd < PLASMA_RUNTIME_FD_FIRST || fd >= PLASMA_RUNTIME_FD_FIRST + PLASMA_RUNTIME_FD_COUNT)\n"
        "        return 0;\n")

    # Per-process low descriptor aliases.
    text=rep(text,
        "    struct plasma_runtime_fd runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n",
        "    struct plasma_runtime_fd runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n"
        "    struct plasma_runtime_fd low_runtime_fds[10];\n")
    text=rep(text,
        "    bytes_copy(process->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n",
        "    bytes_copy(process->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n"
        "    bytes_copy(process->low_runtime_fds, plasma_low_runtime_fds, sizeof(plasma_low_runtime_fds));\n")
    text=rep(text,
        "    bytes_copy(plasma_runtime_fds, process->runtime_fds, sizeof(plasma_runtime_fds));\n",
        "    bytes_copy(plasma_runtime_fds, process->runtime_fds, sizeof(plasma_runtime_fds));\n"
        "    bytes_copy(plasma_low_runtime_fds, process->low_runtime_fds, sizeof(plasma_low_runtime_fds));\n")
    text=rep(text,
        "    bytes_copy(child->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n",
        "    bytes_copy(child->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));\n"
        "    bytes_copy(child->low_runtime_fds, plasma_low_runtime_fds, sizeof(plasma_low_runtime_fds));\n")

    # Low descriptors overridden by runtime aliases are no longer TTYs.
    text=rep(text,
        "    return fd >= 0 && fd <= 9 && fd != PLASMA_FB_FD;\n",
        "    return fd >= 0 && fd <= 9 && fd != PLASMA_FB_FD && !plasma_low_runtime_fds[fd].used;\n")

    # socketpair may be the first runtime operation in a process.
    text=rep(text,
        "static int64_t plasma_socketpair(int domain, int type, int protocol, uint64_t pair_address) {\n"
        "    (void)protocol;\n",
        "static int64_t plasma_socketpair(int domain, int type, int protocol, uint64_t pair_address) {\n"
        "    (void)protocol;\n    plasma_runtime_init();\n")

    # dup/dup2/dup3 for runtime descriptors and clearing aliases when a tty is duplicated.
    old='''    case SYS_DUP:
        return fd_is_tty((int)a1) ? 3 : -LINUX_EBADF;
    case SYS_DUP2:
    case SYS_DUP3:
        return fd_is_tty((int)a1) && (int)a2 >= 0 && (int)a2 <= 9 ? (int64_t)a2 : -LINUX_EBADF;
'''
    new='''    case SYS_DUP: {
        struct plasma_runtime_fd *runtime = plasma_runtime_fd((int)a1);
        if (runtime != 0) {
            int fd = plasma_alloc_runtime_fd(runtime->type, runtime->object, runtime->flags);
            if (fd >= 0) plasma_runtime_fds[fd - PLASMA_RUNTIME_FD_FIRST].offset = runtime->offset;
            return fd;
        }
        return fd_is_tty((int)a1) ? 3 : -LINUX_EBADF;
    }
    case SYS_DUP2:
    case SYS_DUP3: {
        const int source = (int)a1, target = (int)a2;
        if (target < 0) return -LINUX_EBADF;
        struct plasma_runtime_fd *runtime = plasma_runtime_fd(source);
        if (runtime != 0) {
            if (target >= 0 && target < 10) {
                plasma_low_runtime_fds[target] = *runtime;
                return target;
            }
            if (target >= PLASMA_RUNTIME_FD_FIRST && target < PLASMA_RUNTIME_FD_FIRST + PLASMA_RUNTIME_FD_COUNT) {
                plasma_runtime_fds[target - PLASMA_RUNTIME_FD_FIRST] = *runtime;
                return target;
            }
            return -LINUX_EBADF;
        }
        if (fd_is_tty(source) && target >= 0 && target < 10) {
            bytes_zero(&plasma_low_runtime_fds[target], sizeof(plasma_low_runtime_fds[target]));
            return target;
        }
        return -LINUX_EBADF;
    }
'''
    text=rep(text,old,new)

    p.write_text(text,encoding="utf-8"); print(f"Finalized low-fd dup2/dup3 runtime aliases: {p}"); return 0
if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e: print(f"ERROR: {e}",file=sys.stderr); raise SystemExit(1)
