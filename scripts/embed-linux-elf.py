#!/usr/bin/env python3
"""Embed a finished Linux ELF file as a byte array for the early loader test."""

from __future__ import annotations

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT_ELF OUTPUT_C", file=sys.stderr)
        return 2

    input_path = pathlib.Path(sys.argv[1])
    output_path = pathlib.Path(sys.argv[2])
    data = input_path.read_bytes()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "const uint8_t twilight_linux_hello_elf[] = {",
    ]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines += [
        "};",
        f"const size_t twilight_linux_hello_elf_size = {len(data)}u;",
        "",
    ]
    output_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"Embedded Linux ELF: {input_path} ({len(data)} bytes) -> {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
