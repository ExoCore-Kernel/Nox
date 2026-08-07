#!/usr/bin/env python3
"""Fetch Linux v2.6.24's upstream 8139too.c without modifying it."""

from __future__ import annotations

import hashlib
import pathlib
import sys
import urllib.request

URL = "https://raw.githubusercontent.com/torvalds/linux/v2.6.24/drivers/net/8139too.c"
EXPECTED_GIT_BLOB_SHA1 = "eef6fecfff2ac731eb7259896ea8853adc384c8c"


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

    actual = git_blob_sha1(data)
    if actual != EXPECTED_GIT_BLOB_SHA1:
        print("error: downloaded 8139too.c does not match pinned upstream Git blob", file=sys.stderr)
        print(f"expected: {EXPECTED_GIT_BLOB_SHA1}", file=sys.stderr)
        print(f"actual:   {actual}", file=sys.stderr)
        return 1

    output.write_bytes(data)
    print(f"Fetched unmodified Linux v2.6.24 8139too.c: {len(data)} bytes")
    print(f"Verified upstream Git blob SHA-1: {actual}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
