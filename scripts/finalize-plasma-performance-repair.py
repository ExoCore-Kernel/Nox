#!/usr/bin/env python3
"""Repair and validate generated C after the Plasma performance pass."""
from __future__ import annotations

import pathlib
import sys


def validate_no_multiline_c_strings(text: str) -> None:
    in_string = False
    in_char = False
    in_block_comment = False
    in_line_comment = False
    escaped = False
    line = 1
    i = 0
    while i < len(text):
        ch = text[i]
        nxt = text[i + 1] if i + 1 < len(text) else ""

        if in_line_comment:
            if ch == "\n":
                in_line_comment = False
                line += 1
            i += 1
            continue

        if in_block_comment:
            if ch == "*" and nxt == "/":
                in_block_comment = False
                i += 2
                continue
            if ch == "\n":
                line += 1
            i += 1
            continue

        if in_string:
            if ch == "\n":
                if escaped:
                    escaped = False
                    line += 1
                    i += 1
                    continue
                raise RuntimeError(
                    f"generated C contains unterminated string literal before line {line}"
                )
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            i += 1
            continue

        if in_char:
            if ch == "\n":
                raise RuntimeError(
                    f"generated C contains unterminated character literal before line {line}"
                )
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == "'":
                in_char = False
            i += 1
            continue

        if ch == "/" and nxt == "/":
            in_line_comment = True
            i += 2
            continue
        if ch == "/" and nxt == "*":
            in_block_comment = True
            i += 2
            continue
        if ch == '"':
            in_string = True
            escaped = False
            i += 1
            continue
        if ch == "'":
            in_char = True
            escaped = False
            i += 1
            continue
        if ch == "\n":
            line += 1
        i += 1

    if in_string:
        raise RuntimeError("generated C ends inside a string literal")
    if in_char:
        raise RuntimeError("generated C ends inside a character literal")
    if in_block_comment:
        raise RuntimeError("generated C ends inside a block comment")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # re.sub replacement strings interpret backslash escapes. The performance
    # pass therefore produced one physical newline inside the ENOSYS C string.
    broken = '    serial_write(" -> -ENOSYS\n");\n'
    fixed = '    serial_write(" -> -ENOSYS\\n");\n'
    repaired_string = broken in text
    if repaired_string:
        text = text.replace(broken, fixed, 1)

    # Whole-object assignments are the one standalone `image` syntax not caught
    # by the pointer conversion. Assign through the selected image instead.
    whole_image = "image = (struct shell_image){0};"
    whole_count = text.count(whole_image)
    if whole_count:
        text = text.replace(
            whole_image,
            "*plasma_active_image = (struct shell_image){0};",
        )

    for fragment in ("\n    image = ", "\n        image = ", "\nimage = "):
        if fragment in text:
            raise RuntimeError(
                f"legacy standalone image assignment survived performance pass: {fragment!r}"
            )

    validate_no_multiline_c_strings(text)
    path.write_text(text, encoding="utf-8")
    print(
        "Validated Plasma performance-generated C: "
        f"repaired-enosys-string={int(repaired_string)} "
        f"whole-image-assignments={whole_count}: {path}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
