#!/usr/bin/env python3
"""Patch the Plasma CPIO Xorg config to use depth 24 on a 32bpp framebuffer.

Xorg's fbdev driver represents the common RGB888-in-32bpp framebuffer as
Depth 24, framebuffer bpp 32.  Asking it for Depth 32 leaves the RGB weight as
000 and causes xf86-video-fbdev to reject the screen before ScreenInit.

The replacement is deliberately byte-for-byte the same length, so it is safe
to apply directly to a newc CPIO archive without rewriting headers or offsets.
This also lets PLASMA_REUSE_ROOTFS=1 pick up the fix without rebuilding the
~1.7 GiB rootfs.
"""
from __future__ import annotations

import mmap
import pathlib
import sys

OLD = b"    DefaultDepth 32\n"
NEW = b"    DefaultDepth 24\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} ROOTFS.cpio", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    if not path.is_file():
        raise RuntimeError(f"rootfs not found: {path}")

    with path.open("r+b") as file:
        with mmap.mmap(file.fileno(), 0) as image:
            count = 0
            cursor = 0
            while True:
                offset = image.find(OLD, cursor)
                if offset < 0:
                    break
                image[offset : offset + len(OLD)] = NEW
                count += 1
                cursor = offset + len(NEW)
            image.flush()

    if count == 0:
        # Idempotent success if this archive was already patched by a prior run.
        data = path.read_bytes()
        if NEW in data:
            print(f"Plasma fbdev depth already fixed in {path}")
            return 0
        raise RuntimeError("DefaultDepth 32 Xorg config was not found in Plasma rootfs")

    print(f"Patched Plasma Xorg fbdev depth 32 -> 24 in {path} ({count} occurrence(s))")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
