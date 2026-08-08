#!/usr/bin/env python3
"""Final small fixups after rootfs injection into generated Bash ABI C."""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    marker = "#define ROOTFS_FD_FIRST 10\n"
    if marker not in text:
        raise RuntimeError("rootfs Bash ABI injection marker not found")

    declarations = (
        "static bool user_range(uint64_t address, uint64_t length, bool writable);\n"
        "static bool user_copy_out(uint64_t address, const void *source, uint64_t length);\n\n"
    )
    if declarations not in text:
        text = text.replace(marker, declarations + marker, 1)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma Bash compatibility unit: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
