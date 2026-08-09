#!/usr/bin/env python3
"""Keep large process-image copies explicit in the freestanding Plasma ABI."""

from __future__ import annotations

import pathlib
import sys


def require_replace(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected process fragment {old!r} at least {count} time(s), found {found}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = require_replace(
        text,
        "    *victim = (struct shell_image){0};\n",
        "    bytes_zero(victim, sizeof(*victim));\n",
    )
    text = require_replace(
        text,
        "    *child = *parent;\n",
        "    bytes_copy(child, parent, sizeof(*child));\n",
    )
    text = require_replace(
        text,
        "    plasma_parent_image = image;\n",
        "    bytes_copy(&plasma_parent_image, &image, sizeof(image));\n",
    )
    text = require_replace(
        text,
        "    image = plasma_parent_image;\n",
        "    bytes_copy(&image, &plasma_parent_image, sizeof(image));\n",
    )
    text = require_replace(
        text,
        "    plasma_parent_image = (struct shell_image){0};\n",
        "    bytes_zero(&plasma_parent_image, sizeof(plasma_parent_image));\n",
    )
    text = require_replace(
        text,
        "    plasma_child_build_image = (struct shell_image){0};\n",
        "    bytes_zero(&plasma_child_build_image, sizeof(plasma_child_build_image));\n",
        1,
    )
    text = require_replace(
        text,
        "    image = plasma_child_build_image;\n",
        "    bytes_copy(&image, &plasma_child_build_image, sizeof(image));\n",
    )
    text = require_replace(
        text,
        "    plasma_child_build_image = (struct shell_image){0};\n",
        "    bytes_zero(&plasma_child_build_image, sizeof(plasma_child_build_image));\n",
        1,
    )

    zombie_assignment = "    plasma_zombie_image = image;\n"
    zombie_count = text.count(zombie_assignment)
    if zombie_count == 0:
        raise RuntimeError("expected at least one zombie image assignment")
    text = text.replace(
        zombie_assignment,
        "    bytes_copy(&plasma_zombie_image, &image, sizeof(image));\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized freestanding process image copies: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
