#!/usr/bin/env python3
"""Add useful lifetime diagnostics for the real Xorg process.

The ordinary post-loader trace is intentionally bounded so serial output does
not destroy performance under TCG.  Once that trace budget is exhausted, this
finalizer keeps a very low-rate heartbeat for Xorg and gives special visibility
to poll/epoll waits.  This lets headless bring-up distinguish three cases:

* Xorg reached its normal event loop and is repeatedly yielding.
* Xorg entered one wait syscall and never returned from the kernel.
* Xorg stopped making syscalls entirely (userspace spin/fault path).

The diagnostics are syscall-count based rather than timer based because the
current Twilight Plasma scheduler is cooperative and has no periodic userspace
preemption yet.
"""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one Xorg trace anchor, found {count}: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    anchor = (
        "static int64_t shell_dispatch(uint64_t number,\n"
        "                              uint64_t a1, uint64_t a2, uint64_t a3,\n"
        "                              uint64_t a4, uint64_t a5, uint64_t a6) {\n"
    )

    helper = r'''static bool plasma_xorg_syscall_trace_armed;
static uint32_t plasma_xorg_syscall_trace_count;
static uint64_t plasma_xorg_lifetime_syscalls;
static uint64_t plasma_xorg_wait_syscalls;
static uint32_t plasma_xorg_membarrier_count;

static bool plasma_xorg_process_active(void) {
    struct plasma_process *process = plasma_current_process();
    if (process == 0) return false;
    return string_equal(process->exec_path, "/usr/libexec/Xorg") ||
           string_equal(process->exec_path, "/usr/bin/Xorg");
}

static void plasma_xorg_trace_path(const char *label, uint64_t address) {
    char path[192];
    if (address == 0 || !copy_user_string(address, path, sizeof(path))) return;
    serial_write(" ");
    serial_write(label);
    serial_write("=");
    serial_write(path);
}

static bool plasma_xorg_wait_syscall(uint64_t number) {
    return number == SYS_POLL || number == SYS_PPOLL ||
           number == SYS_SELECT || number == SYS_PSELECT6 ||
           number == SYS_EPOLL_WAIT || number == SYS_EPOLL_PWAIT;
}

static void plasma_xorg_debug_wait_entry(uint64_t number,
                                         uint64_t a1, uint64_t a2,
                                         uint64_t a3, uint64_t a4) {
    if (!plasma_xorg_process_active()) return;
    ++plasma_xorg_wait_syscalls;
    if (plasma_xorg_wait_syscalls <= 12u ||
        (plasma_xorg_wait_syscalls & 4095u) == 0u) {
        serial_write("[linux:xorg-debug] wait-enter #");
        serial_u64(plasma_xorg_wait_syscalls);
        serial_write(" syscall="); serial_u64(number);
        serial_write(" a1="); serial_u64(a1);
        serial_write(" a2="); serial_u64(a2);
        serial_write(" a3="); serial_u64(a3);
        serial_write(" a4="); serial_u64(a4);
        serial_write("\n");
    }
}

static void plasma_xorg_debug_wait_return(uint64_t number, int64_t result) {
    if (!plasma_xorg_process_active()) return;
    if (plasma_xorg_wait_syscalls <= 12u ||
        (plasma_xorg_wait_syscalls & 4095u) == 0u) {
        serial_write("[linux:xorg-debug] wait-return syscall=");
        serial_u64(number);
        serial_write(" rc=");
        if (result < 0) {
            serial_write("-");
            serial_u64((uint64_t)(-result));
        } else {
            serial_u64((uint64_t)result);
        }
        serial_write("\n");
    }
}

static int64_t plasma_xorg_debug_epoll_wait(uint64_t number,
                                            int epfd, uint64_t events_address,
                                            int maxevents, int timeout) {
    plasma_xorg_debug_wait_entry(number, (uint64_t)epfd, events_address,
                                 (uint64_t)maxevents, (uint64_t)(uint32_t)timeout);
    int64_t result = plasma_epoll_wait(epfd, events_address, maxevents, timeout);
    plasma_xorg_debug_wait_return(number, result);
    return result;
}

static void plasma_xorg_trace_syscall(uint64_t number,
                                      uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a6;
    if (!plasma_xorg_process_active()) return;

    /* Xorg/libudev performs socket(AF_NETLINK=16, ..., NETLINK_KOBJECT_UEVENT=15)
     * immediately before the banner on this bring-up path. Arm here so the
     * detailed budget is spent on configuration and driver initialization. */
    if (!plasma_xorg_syscall_trace_armed) {
        if (number != SYS_SOCKET || a1 != 16u) return;
        plasma_xorg_syscall_trace_armed = true;
        plasma_xorg_syscall_trace_count = 0;
        plasma_xorg_lifetime_syscalls = 0;
        plasma_xorg_wait_syscalls = 0;
        serial_write("[linux:xorg-syscall] trace armed after AF_NETLINK udev probe\n");
    }

    ++plasma_xorg_lifetime_syscalls;

    /* A sparse lifetime heartbeat remains after the detailed trace is exhausted.
     * Seeing these lines means Xorg is alive and repeatedly returning to the
     * kernel rather than being frozen in userspace. */
    if ((plasma_xorg_lifetime_syscalls & 16383u) == 0u) {
        serial_write("[linux:xorg-debug] heartbeat syscalls=");
        serial_u64(plasma_xorg_lifetime_syscalls);
        serial_write(" current=");
        serial_u64(number);
        serial_write(" waits=");
        serial_u64(plasma_xorg_wait_syscalls);
        serial_write("\n");
    }

    /* x86_64 syscall 324 is membarrier. Keep its arguments visible even after
     * the normal trace budget so we can tell QUERY from a real barrier command. */
    if (number == 324u && plasma_xorg_membarrier_count < 8u) {
        ++plasma_xorg_membarrier_count;
        serial_write("[linux:xorg-debug] membarrier command=");
        serial_u64(a1);
        serial_write(" flags=");
        serial_u64(a2);
        serial_write(" cpu_id=");
        serial_u64(a3);
        serial_write("\n");
    }

    /* poll/select are implemented outside the epoll helper, so record their
     * entries here. epoll entry+return are logged by the wrapper below. */
    if (plasma_xorg_wait_syscall(number) &&
        number != SYS_EPOLL_WAIT && number != SYS_EPOLL_PWAIT) {
        plasma_xorg_debug_wait_entry(number, a1, a2, a3, a4);
    }

    if (plasma_xorg_syscall_trace_count >= 512u) return;

    serial_write("[linux:xorg-syscall] #");
    serial_u64((uint64_t)plasma_xorg_syscall_trace_count++);
    serial_write(" n=");
    serial_u64(number);
    serial_write(" a1=");
    serial_u64(a1);
    serial_write(" a2=");
    serial_u64(a2);
    serial_write(" a3=");
    serial_u64(a3);
    serial_write(" a4=");
    serial_u64(a4);

    switch (number) {
    case SYS_OPEN:
    case SYS_STAT:
    case SYS_LSTAT:
    case SYS_ACCESS:
    case SYS_CHDIR:
    case SYS_UNLINK:
    case SYS_MKDIR:
    case SYS_READLINK:
        plasma_xorg_trace_path("path", a1);
        break;
    case SYS_OPENAT:
    case SYS_NEWFSTATAT:
    case SYS_MKDIRAT:
    case SYS_UNLINKAT:
    case SYS_READLINKAT:
        plasma_xorg_trace_path("path", a2);
        break;
    case SYS_LINK:
        plasma_xorg_trace_path("old", a1);
        plasma_xorg_trace_path("new", a2);
        break;
    case SYS_IOCTL:
        serial_write(" request=");
        serial_u64(a2);
        break;
    case SYS_MMAP:
        serial_write(" len=");
        serial_u64(a2);
        serial_write(" fd=");
        serial_u64(a5);
        break;
    default:
        break;
    }
    serial_write("\n");
}

'''

    text = replace_once(
        text,
        anchor,
        helper + anchor + "    plasma_xorg_trace_syscall(number, a1, a2, a3, a4, a5, a6);\n",
    )

    # epoll_wait currently returns directly, which hides whether the wait helper
    # returned or trapped inside it. Route only epoll waits through the debug
    # wrapper; semantics are otherwise identical.
    text = replace_once(
        text,
        "    case SYS_EPOLL_WAIT: return plasma_epoll_wait((int)a1, a2, (int)a3, (int)a4);\n",
        "    case SYS_EPOLL_WAIT: return plasma_xorg_debug_epoll_wait(SYS_EPOLL_WAIT, (int)a1, a2, (int)a3, (int)a4);\n",
    )
    text = replace_once(
        text,
        "    case SYS_EPOLL_PWAIT: return plasma_epoll_wait((int)a1, a2, (int)a3, (int)a4);\n",
        "    case SYS_EPOLL_PWAIT: return plasma_xorg_debug_epoll_wait(SYS_EPOLL_PWAIT, (int)a1, a2, (int)a3, (int)a4);\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Added Xorg detailed trace + event-loop heartbeat diagnostics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
