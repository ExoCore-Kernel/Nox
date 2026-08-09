#!/usr/bin/env python3
"""Raise the per-process userspace mapping budget for Plasma bring-up only.

The tiny BusyBox shell deliberately tracks only 768 mapped pages (~3 MiB).
That is enough for the shell but Xorg/Qt shared objects exceed it quickly, and
musl then reports misleading dlopen failures such as "Out of memory" or
"module does not exist".  Plasma gets a larger fixed descriptor budget while
normal Twilight builds keep the small shell limit.
"""

from __future__ import annotations

import pathlib
import sys

OLD = "#define SHELL_MAX_PAGES     768u\n"
NEW = "#define SHELL_MAX_PAGES     8192u\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    count = text.count(OLD)
    if count != 1:
        raise RuntimeError(f"expected exactly one shell page-budget fragment, found {count}")

    text = text.replace(OLD, NEW, 1)
    path.write_text(text, encoding="utf-8")
    print(f"Expanded Plasma userspace page budget: {path} (768 -> 8192 pages)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
