#!/usr/bin/env python3
"""Fetch BusyBox.org's official static x86_64-musl BusyBox test binary.

The downloaded executable is never rewritten or patched.  Twilight embeds the
finished ELF byte-for-byte only as transport until a filesystem is available.
"""

from __future__ import annotations

import hashlib
import pathlib
import sys
import urllib.request

URL = "https://busybox.net/downloads/binaries/1.35.0-x86_64-linux-musl/busybox"
EXPECTED_SIZE = 1_131_168


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)

    request = urllib.request.Request(
        URL,
        headers={"User-Agent": "Twilight-BusyBox-compat-test/1.0"},
    )
    print(f"Fetching unmodified BusyBox 1.35.0 x86_64-musl from {URL}")
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()

    if len(data) != EXPECTED_SIZE:
        print(
            f"ERROR: BusyBox size changed: got {len(data)}, expected {EXPECTED_SIZE}",
            file=sys.stderr,
        )
        return 1
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        print("ERROR: downloaded BusyBox is not a little-endian ELF64 file", file=sys.stderr)
        return 1
    # e_machine is little-endian uint16 at ELF64 offset 18; 62 is EM_X86_64.
    if int.from_bytes(data[18:20], "little") != 62:
        print("ERROR: downloaded BusyBox is not x86_64", file=sys.stderr)
        return 1

    output.write_bytes(data)
    digest = hashlib.sha256(data).hexdigest()
    print(f"Fetched official BusyBox unchanged: {output} ({len(data)} bytes)")
    print(f"BusyBox SHA-256: {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
