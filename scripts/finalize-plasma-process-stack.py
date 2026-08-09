#!/usr/bin/env python3
"""Move large scheduler image scratch state out of the syscall stack.

The GUI finalizer later raises SHELL_MAX_PAGES for Qt/KDE.  That makes
struct shell_image hundreds of KiB, far larger than Twilight's 16 KiB shared
SYSCALL transition stack.  The cooperative scheduler is single-CPU and
non-reentrant at this bring-up stage, so one static scratch image is sufficient
for the two short metadata swaps performed during wait wakeups and child-tid
initialization.
"""

from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected scheduler-stack fragment {old[:140]!r}; found {found}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "static struct shell_image plasma_child_build_image;\n",
        "static struct shell_image plasma_child_build_image;\n"
        "static struct shell_image plasma_scheduler_scratch_image;\n",
    )

    old = (
        "        struct shell_image saved_image;\n"
        "        bytes_copy(&saved_image, &image, sizeof(image));\n"
    )
    new = (
        "        bytes_copy(&plasma_scheduler_scratch_image, &image, sizeof(image));\n"
    )
    text = rep(text, old, new, 2)

    text = rep(
        text,
        "        bytes_copy(&image, &saved_image, sizeof(image));\n",
        "        bytes_copy(&image, &plasma_scheduler_scratch_image, sizeof(image));\n",
        2,
    )

    path.write_text(text, encoding="utf-8")
    print(f"Moved Plasma scheduler shell_image scratch off transition stack: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
