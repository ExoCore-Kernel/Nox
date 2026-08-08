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
        # Give the sector a conventional boot signature too. The AHCI proof
        # does not depend on it, but it makes sector zero easier to inspect.
        sector[510] = 0x55
        sector[511] = 0xAA
        f.write(sector)

    print(f"Created AHCI test disk: {path} ({SIZE // (1024 * 1024)} MiB)")
    print(f"LBA0 marker: {MARKER.decode('ascii').strip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
