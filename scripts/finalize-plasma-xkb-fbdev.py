#!/usr/bin/env python3
"""Finish the tiny Linux ABI pieces Xorg needs after fbdev screen bring-up.

Adds the legacy pipe(2) syscall used while Xorg spawns xkbcomp and accepts
FBIOPUTCMAP on Twilight's true-colour framebuffer.  Both are bring-up shims:
pipe(2) reuses the existing pipe2 implementation with flags=0, while palette
writes are harmless for the 32-bpp RGB framebuffer and may be acknowledged.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected XKB/fbdev fragment not found: {old[:160]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # x86_64 Linux syscall 22 is pipe(2). Xorg/xkbcomp still uses it on this
    # path even though pipe2(293) is already implemented by the Plasma runtime.
    if "#define SYS_PIPE            22ull\n" not in text:
        text = rep(
            text,
            "#define SYS_ACCESS          21ull\n",
            "#define SYS_ACCESS          21ull\n#define SYS_PIPE            22ull\n",
        )

    if "case SYS_PIPE: return plasma_pipe2(a1, 0u);" not in text:
        text = rep(
            text,
            "    case SYS_PIPE2: return plasma_pipe2(a1, (uint32_t)a2);\n",
            "    case SYS_PIPE: return plasma_pipe2(a1, 0u);\n"
            "    case SYS_PIPE2: return plasma_pipe2(a1, (uint32_t)a2);\n",
        )

    # Linux fb.h: FBIOPUTCMAP == 0x4605. fbdev performs palette writes during
    # server setup even on a 32-bpp true-colour framebuffer. There is no
    # programmable palette behind Twilight's Limine framebuffer, so accepting
    # the request is the correct no-op behaviour for this bring-up ABI.
    if "#define FBIOPUTCMAP 0x4605ull\n" not in text:
        text = rep(
            text,
            "#define FBIOGET_FSCREENINFO 0x4602ull\n",
            "#define FBIOGET_FSCREENINFO 0x4602ull\n#define FBIOPUTCMAP 0x4605ull\n",
        )

    text = rep(
        text,
        "    if (request==FBIOPAN_DISPLAY || request==FBIOBLANK) return 0;\n",
        "    if (request==FBIOPUTCMAP) {\n"
        "        serial_write(\"[linux:fbdev] FBIOPUTCMAP accepted as truecolor no-op\\n\");\n"
        "        return 0;\n"
        "    }\n"
        "    if (request==FBIOPAN_DISPLAY || request==FBIOBLANK) return 0;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Xorg XKB pipe(2) + fbdev colormap ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
