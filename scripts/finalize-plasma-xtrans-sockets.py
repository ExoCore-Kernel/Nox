#!/usr/bin/env python3
"""Finalize Xorg/Xtrans socket bring-up details.

Xtrans historically rejects socket descriptors at or above TRANS_OPEN_MAX when
its select-based transport path is built.  Twilight's rootfs descriptors occupy
10..57, while the writable runtime descriptor pool previously began at 64.
Keep runtime descriptors contiguous by starting them at 58 so Xorg's early
epoll/socket descriptors remain in the traditional low-fd range.

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
    # first listener sockets below legacy select/open-fd ceilings.
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

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Xtrans low runtime FDs + clock_getres ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
