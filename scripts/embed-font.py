#!/usr/bin/env python3
"""Fetch and embed Nox's bring-up console font into a C translation unit."""

from __future__ import annotations

import gzip
import pathlib
import sys
import urllib.request

FONT_URL = "https://raw.githubusercontent.com/powerline/fonts/master/Terminus/PSF/ter-powerline-v16n.psf.gz"


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT.c", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    cache = output.parent / "ter-powerline-v16n.psf.gz"
    output.parent.mkdir(parents=True, exist_ok=True)

    if not cache.exists():
        print("Fetching Terminus-family console font...")
        try:
            with urllib.request.urlopen(FONT_URL, timeout=30) as response:
                cache.write_bytes(response.read())
        except Exception as exc:
            print(f"error: unable to fetch console font: {exc}", file=sys.stderr)
            return 1

    try:
        font = gzip.decompress(cache.read_bytes())
    except Exception as exc:
        print(f"error: unable to decompress console font: {exc}", file=sys.stderr)
        return 1

    lines = [
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "const uint8_t twilight_console_font[] = {",
    ]

    for offset in range(0, len(font), 12):
        chunk = font[offset : offset + 12]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")

    lines += [
        "};",
        "",
        "const size_t twilight_console_font_size = sizeof(twilight_console_font);",
        "",
    ]
    output.write_text("\n".join(lines), encoding="utf-8")
    print(f"Embedded {len(font)} font bytes -> {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
