#!/usr/bin/env python3
"""Finish the zero-copy shell_image pointer conversion.

The original BusyBox/Bash compatibility loader has two whole-object resets of
its global `image` object.  finalize-plasma-performance.py converts field,
address and sizeof uses to plasma_active_image, but these direct assignments are
syntactically distinct.  Convert them after the main performance pass and fail
if the known old assignment form somehow remains.
"""
from __future__ import annotations

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text, count = re.subn(
        r"(?<![>.])\bimage\s*=\s*\(struct shell_image\)\{0\};",
        "*plasma_active_image = (struct shell_image){0};",
        text,
    )
    if count < 2:
        raise RuntimeError(f"expected at least two whole shell_image resets, found {count}")
    if re.search(r"(?<![>.])\bimage\s*=", text):
        raise RuntimeError("residual standalone shell_image assignment remains after performance conversion")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized residual active-image assignments ({count} converted): {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
