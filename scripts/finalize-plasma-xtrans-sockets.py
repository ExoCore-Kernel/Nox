#!/usr/bin/env python3
"""Finalize Xorg/Xtrans socket bring-up details.

Xtrans opens AF_UNIX listeners very early and, in non-poll builds, rejects a
socket whose descriptor is at or above sysconf(_SC_OPEN_MAX).  Keep Twilight's
runtime descriptors low and report a realistic RLIMIT_NOFILE instead of
infinity so that check has normal Linux semantics.

The bring-up socket backend also emits a short serial trace for every AF_UNIX
socket request.  This makes the next failure unambiguous: we can distinguish a
Twilight socket allocation error from a later Xtrans bind/listen problem.

Xorg also probes clock_getres(2) while starting its transport layer.  Provide a
small valid resolution instead of returning ENOSYS.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected Xtrans source fragment not found: {old[:160]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # ROOTFS_FD_FIRST=10 and ROOTFS_FD_COUNT=48 occupy descriptors 10..57.
    # Starting runtime FDs at 58 avoids an unnecessary gap and keeps Xtrans's
    # first listener sockets in the traditional low-fd range.
    text = rep(
        text,
        "#define PLASMA_RUNTIME_FD_FIRST 64\n",
        "#define PLASMA_RUNTIME_FD_FIRST 58\n",
    )

    text = rep(
        text,
        "#define SYS_CLOCK_GETTIME  228ull\n",
        "#define SYS_CLOCK_GETTIME  228ull\n#define SYS_CLOCK_GETRES   229ull\n",
    )

    text = rep(
        text,
        "    case SYS_CLOCK_GETTIME:\n    case SYS_GETTIMEOFDAY:\n",
        "    case SYS_CLOCK_GETRES:\n"
        "        if (a2 != 0) {\n"
        "            if (!user_store_u64(a2, 0) || !user_store_u64(a2 + 8, 1)) return -LINUX_EFAULT;\n"
        "        }\n"
        "        return 0;\n"
        "    case SYS_CLOCK_GETTIME:\n    case SYS_GETTIMEOFDAY:\n",
    )

    # musl implements sysconf(_SC_OPEN_MAX) from RLIMIT_NOFILE.  Returning
    # infinity for every resource is legal-ish for a toy ABI but is a poor fit
    # for Xtrans's descriptor ceiling check.  Give NOFILE the conventional 1024
    # soft/hard limit while leaving the other bring-up resources unlimited.
    old_rlimit = '''    case SYS_PRLIMIT64:\n    case SYS_GETRLIMIT: {\n        const uint64_t out = number == SYS_PRLIMIT64 ? a4 : a2;\n        if (out != 0) {\n            uint64_t limits[2] = { UINT64_MAX, UINT64_MAX };\n            if (!user_copy_out(out, limits, sizeof(limits))) return -LINUX_EFAULT;\n        }\n        return 0;\n    }\n'''
    new_rlimit = '''    case SYS_PRLIMIT64:\n    case SYS_GETRLIMIT: {\n        const uint64_t resource = number == SYS_PRLIMIT64 ? a2 : a1;\n        const uint64_t out = number == SYS_PRLIMIT64 ? a4 : a2;\n        if (out != 0) {\n            uint64_t limits[2] = { UINT64_MAX, UINT64_MAX };\n            if (resource == 7u) { /* RLIMIT_NOFILE */\n                limits[0] = 1024u;\n                limits[1] = 1024u;\n            }\n            if (!user_copy_out(out, limits, sizeof(limits))) return -LINUX_EFAULT;\n        }\n        return 0;\n    }\n'''
    text = rep(text, old_rlimit, new_rlimit)

    old_socket = '''static int64_t plasma_socket_create(int domain, int type, int protocol) {\n    (void)protocol;\n    plasma_runtime_init();\n    if (domain != PLASMA_AF_UNIX) return -LINUX_EAFNOSUPPORT;\n    if ((type & 0xf) != PLASMA_SOCK_STREAM) return -LINUX_ENOSYS;\n    int object = plasma_alloc_socket_object();\n    if (object < 0) return -LINUX_ENOMEM;\n    return plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)object, (uint32_t)type);\n}\n'''
    new_socket = r'''static void plasma_socket_trace_number(const char *label, int64_t value) {
    serial_write("[linux:socket] ");
    serial_write(label);
    serial_write("=");
    if (value < 0) {
        serial_write_char('-');
        value = -value;
    }
    char reverse[24];
    size_t count = 0;
    do {
        reverse[count++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(reverse));
    while (count != 0) serial_write_char(reverse[--count]);
    serial_write("\n");
}

static int64_t plasma_socket_create(int domain, int type, int protocol) {
    plasma_runtime_init();
    plasma_socket_trace_number("domain", domain);
    plasma_socket_trace_number("type", type);
    plasma_socket_trace_number("protocol", protocol);

    if (domain != PLASMA_AF_UNIX) {
        serial_write("[linux:socket] reject: only AF_UNIX is implemented\n");
        return -LINUX_EAFNOSUPPORT;
    }

    /* Xtrans normally asks for SOCK_STREAM, optionally ORed with Linux socket
     * flags.  During bring-up, treat any AF_UNIX socket as stream semantics so
     * an unexpected flag encoding does not turn into a misleading ENOSYS. */
    if ((type & 0xf) != PLASMA_SOCK_STREAM)
        serial_write("[linux:socket] non-stream base type accepted as bring-up stream\n");

    int object = plasma_alloc_socket_object();
    if (object < 0) {
        serial_write("[linux:socket] allocation failed: socket object table full\n");
        return -LINUX_ENOMEM;
    }

    int fd = plasma_alloc_runtime_fd(PLASMA_RT_SOCKET, (uint16_t)object, (uint32_t)type);
    plasma_socket_trace_number("fd", fd);
    if (fd < 0)
        bytes_zero(&plasma_sockets[object], sizeof(plasma_sockets[object]));
    return fd;
}
'''
    text = rep(text, old_socket, new_socket)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Xtrans sockets + RLIMIT_NOFILE + clock_getres ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
