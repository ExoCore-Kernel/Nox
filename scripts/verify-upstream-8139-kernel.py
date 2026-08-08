#!/usr/bin/env python3
"""Refuse to boot a supposed upstream-8139 test kernel unless it is genuine."""

from __future__ import annotations

import pathlib
import sys

REQUIRED = (
    b"8139too Fast Ethernet driver 0.9.28",
    b'Use the "8139cp" driver for improved performance and stability.',
    b"STRICT Linux driver test: byte-for-byte upstream Linux v2.6.24 8139too.c",
)

FORBIDDEN = (
    b"Linux 8139too core bound",
    b"RTL8139 opened: RX ring DMA=",
    b"first Ethernet frame received through RTL8139 RX DMA",
)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} KERNEL_ELF", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    try:
        data = path.read_bytes()
    except OSError as error:
        print(f"error: unable to read strict-test kernel {path}: {error}", file=sys.stderr)
        return 1

    missing = [marker for marker in REQUIRED if marker not in data]
    forbidden = [marker for marker in FORBIDDEN if marker in data]

    if missing or forbidden:
        print("error: refusing to boot: linked kernel is not a clean upstream-8139 test", file=sys.stderr)
        for marker in missing:
            print(f"  missing required marker: {marker.decode('ascii', 'replace')}", file=sys.stderr)
        for marker in forbidden:
            print(f"  contains Twilight port marker: {marker.decode('ascii', 'replace')}", file=sys.stderr)
        return 1

    print("Verified linked kernel contains upstream Linux v2.6.24 8139too and excludes Twilight's RTL8139 port")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
