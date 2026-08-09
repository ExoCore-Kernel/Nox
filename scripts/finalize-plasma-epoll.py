#!/usr/bin/env python3
"""Finalize declaration ordering for the generated Plasma epoll helpers."""
from __future__ import annotations
import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")
    anchor = "static int plasma_alloc_epoll_object(void) {\n"
    if anchor not in text:
        raise RuntimeError("Plasma epoll helper anchor not found")
    declaration = "static bool fd_is_tty(int fd);\n\n"
    if declaration not in text:
        text = text.replace(anchor, declaration + anchor, 1)
    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma epoll helper declarations: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
