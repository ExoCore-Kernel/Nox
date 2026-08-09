#!/usr/bin/env python3
"""Finish and validate the zero-copy shell_image pointer conversion.

The main performance finalizer intentionally runs last and performs broad source
rewrites. This cleanup handles syntactically distinct whole-object image resets,
repairs the known Python re.sub replacement-string backslash edge case, and then
lexes the generated C to reject raw newlines inside C string/character literals
before Clang sees the file.
"""
from __future__ import annotations

import pathlib
import re
import sys


def validate_literals(text: str) -> None:
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
                    f"generated C has unterminated string literal before line {line}"
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
                    f"generated C has unterminated character literal before line {line}"
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

    # finalize-plasma-performance.py constructs log_unknown() with re.sub().
    # Python interprets backslash escapes in a replacement string, so its \\n    # became one physical newline inside the emitted C string. Repair exactly
    # that generated form here; this is an actual syntax correction, not a stub.
    broken_enosys = '    serial_write(" -> -ENOSYS\n");\n'
    fixed_enosys = '    serial_write(" -> -ENOSYS\\n");\n'
    enosys_repairs = text.count(broken_enosys)
    if enosys_repairs != 1:
        raise RuntimeError(
            f"expected exactly one malformed ENOSYS trace string, found {enosys_repairs}"
        )
    text = text.replace(broken_enosys, fixed_enosys, 1)

    # The pointer conversion handles image.foo, &image, and sizeof(image), while
    # whole-object assignments have none of those forms. Convert the two loader
    # resets through the selected active-image pointer.
    text, image_count = re.subn(
        r"(?<![>.])\bimage\s*=\s*\(struct shell_image\)\{0\};",
        "*plasma_active_image = (struct shell_image){0};",
        text,
    )
    if image_count < 2:
        raise RuntimeError(
            f"expected at least two whole shell_image resets, found {image_count}"
        )
    if re.search(r"(?<![>.])\bimage\s*=", text):
        raise RuntimeError(
            "residual standalone shell_image assignment remains after performance conversion"
        )

    validate_literals(text)

    path.write_text(text, encoding="utf-8")
    print(
        "Validated Plasma performance output: "
        f"ENOSYS strings repaired={enosys_repairs}, "
        f"active-image resets converted={image_count}: {path}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
