#!/usr/bin/env python3
"""Fetch Linux v2.6.24's upstream 8139too.c without modifying it."""

from __future__ import annotations

import hashlib
import pathlib
import sys
import urllib.request

URL = "https://raw.githubusercontent.com/torvalds/linux/v2.6.24/drivers/net/8139too.c"


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)

    with urllib.request.urlopen(URL, timeout=30) as response:
        data = response.read()

    # These checks make accidental tag/path changes fail loudly. The source
    # bytes themselves are written exactly as received from upstream Linux.
    if b"8139too.c: A RealTek RTL-8139 Fast Ethernet driver for Linux." not in data:
        print("error: download is not Linux 8139too.c", file=sys.stderr)
        return 1
    if b'#define DRV_VERSION\t"0.9.28"' not in data:
        print("error: download is not the pinned Linux v2.6.24 driver", file=sys.stderr)
        return 1

    output.write_bytes(data)
    print(f"Fetched unmodified Linux v2.6.24 8139too.c: {len(data)} bytes")
    print(f"Upstream Git blob SHA-1: {git_blob_sha1(data)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
