#!/usr/bin/env python3
"""Create a small deterministic raw SATA disk for Twilight's AHCI proof."""

from __future__ import annotations

import pathlib
import sys

SIZE = 64 * 1024 * 1024
MARKER = b"NOXAHCI: unmodified Linux ahci.c storage proof\r\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    path.parent.mkdir(parents=True, exist_ok=True)

    with path.open("wb") as f:
        f.truncate(SIZE)
        f.seek(0)
        sector = bytearray(512)
        sector[: len(MARKER)] = MARKER
        # Deliberately do NOT write an MBR 0x55AA signature here. This disk is
        # data-only test media. Marking it bootable can make legacy BIOS choose
        # it ahead of the Limine CD and execute the NOXAHCI marker as boot code,
        # producing a completely silent QEMU boot before Twilight ever starts.
        f.write(sector)

    print(f"Created AHCI test disk: {path} ({SIZE // (1024 * 1024)} MiB)")
    print(f"LBA0 marker: {MARKER.decode('ascii').strip()}")
    print("AHCI test disk intentionally has no BIOS boot signature")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
