#!/usr/bin/env python3
"""Embed a finished Linux ELF file as a byte array for early loader tests."""

from __future__ import annotations

import pathlib
import re
import sys


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(
            f"usage: {sys.argv[0]} INPUT_ELF OUTPUT_C [SYMBOL_BASE]",
            file=sys.stderr,
        )
        return 2

    input_path = pathlib.Path(sys.argv[1])
    output_path = pathlib.Path(sys.argv[2])
    symbol = sys.argv[3] if len(sys.argv) == 4 else "twilight_linux_hello_elf"
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol) is None:
        print(f"invalid C symbol: {symbol}", file=sys.stderr)
        return 2

    data = input_path.read_bytes()
    output_path.parent.mkdir(parents=True, exist_ok=True)

    lines = [
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f"const uint8_t {symbol}[] = {{",
    ]
    for offset in range(0, len(data), 16):
        chunk = data[offset : offset + 16]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines += [
        "};",
        f"const size_t {symbol}_size = {len(data)}u;",
        "",
    ]
    output_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"Embedded Linux ELF unchanged: {input_path} ({len(data)} bytes) -> {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
