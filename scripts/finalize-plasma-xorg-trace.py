#!/usr/bin/env python3
"""Add a bounded syscall trace for the real Xorg process during bring-up.

This is diagnostic-only. It traces the first 256 syscall entries once the
current cooperative process execs /usr/libexec/Xorg, including a few useful
pathname/descriptor details. The trace is intentionally bounded so a polling
loop cannot flood serial forever on slow TCG hosts.
"""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one Xorg trace anchor, found {count}")
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

    helper = r'''static uint32_t plasma_xorg_syscall_trace_count;

static void plasma_xorg_trace_path(const char *label, uint64_t address) {
    char path[192];
    if (address == 0 || !copy_user_string(address, path, sizeof(path))) return;
    serial_write(" ");
    serial_write(label);
    serial_write("=");
    serial_write(path);
}

static void plasma_xorg_trace_syscall(uint64_t number,
                                      uint64_t a1, uint64_t a2, uint64_t a3,
                                      uint64_t a4, uint64_t a5, uint64_t a6) {
    (void)a4;
    (void)a6;
    struct plasma_process *process = plasma_current_process();
    if (process == 0 || !string_equal(process->exec_path, "/usr/libexec/Xorg")) return;
    if (plasma_xorg_syscall_trace_count >= 256u) return;

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

    path.write_text(text, encoding="utf-8")
    print(f"Added bounded Xorg syscall trace: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
