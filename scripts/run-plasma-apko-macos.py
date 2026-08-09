#!/usr/bin/env python3
"""Run the native apko Plasma builder with macOS-safe regular-file reads.

Some Alpine files (notably /bin/bbsuid from busybox-suid) are intentionally
installed without owner read permission.  The Plasma CPIO packer records the
Linux mode before opening each regular file, so this wrapper only needs to make
the host-side open succeed.  It temporarily adds S_IRUSR, opens the file, then
restores the original mode immediately; the already-open descriptor remains
readable and the CPIO retains Alpine's original permissions.
"""
from __future__ import annotations

import os
import pathlib
import runpy
import stat
import sys


_original_open = pathlib.Path.open


def macos_safe_open(self: pathlib.Path, mode: str = "r", buffering: int = -1,
                    encoding=None, errors=None, newline=None):
    try:
        return _original_open(self, mode, buffering, encoding, errors, newline)
    except PermissionError:
        # Only relax host-side reads of regular files.  Do not interfere with
        # writes, directories, symlinks, sockets, or other special entries.
        if "r" not in mode or any(flag in mode for flag in ("w", "a", "x", "+")):
            raise
        st = self.lstat()
        if not stat.S_ISREG(st.st_mode):
            raise

        original_mode = stat.S_IMODE(st.st_mode)
        os.chmod(self, original_mode | stat.S_IRUSR)
        try:
            handle = _original_open(self, mode, buffering, encoding, errors, newline)
        finally:
            os.chmod(self, original_mode)
        print(f"Temporarily opened restrictive Alpine file for CPIO: {self}")
        return handle


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT.cpio", file=sys.stderr)
        return 2

    pathlib.Path.open = macos_safe_open
    builder = pathlib.Path(__file__).with_name("fetch-alpine-plasma-x11-apko.py")
    sys.argv = [str(builder), sys.argv[1]]
    runpy.run_path(str(builder), run_name="__main__")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
