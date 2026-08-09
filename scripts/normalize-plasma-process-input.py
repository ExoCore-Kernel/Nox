#!/usr/bin/env python3
"""Normalize generated Linux ABI text before the Plasma process transform.

Several GUI bring-up transforms intentionally edit the syscall dispatcher before
add-plasma-processes.py runs.  The process transform used to require the exact
single-process SYS_EXIT/SYS_EXIT_GROUP body produced by the base Bash ABI.  That
made the pipeline order-sensitive: a harmless earlier edit inside that case
caused the scheduler transform to abort even though the dispatcher was valid.

Keep the process transform simple by canonicalizing only the exit-case region
immediately before it runs.  The following add-plasma-processes.py step then
replaces this canonical single-process handler with plasma_process_exit().
"""

from __future__ import annotations

import pathlib
import sys


CANONICAL_EXIT = '''    case SYS_EXIT:\n    case SYS_EXIT_GROUP:\n        shell_exit_status = (int)(a1 & 0xffu);\n        shell_exit_seen = true;\n        write_msr(IA32_FS_BASE_MSR, 0);\n        fs_base = 0;\n        return LINUX_EXIT_SENTINEL;\n'''


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    start_marker = "    case SYS_EXIT:\n"
    end_marker = "    case SYS_ARCH_PRCTL:\n"
    start = text.find(start_marker)
    if start < 0:
        raise RuntimeError("SYS_EXIT dispatcher case not found")
    end = text.find(end_marker, start)
    if end < 0:
        raise RuntimeError("SYS_ARCH_PRCTL case not found after SYS_EXIT")

    # Refuse to rewrite an unexpectedly huge region.  This protects us from
    # masking a genuinely broken transform that accidentally moved unrelated
    # syscall cases into the exit block.
    existing = text[start:end]
    if len(existing) > 1200:
        raise RuntimeError(
            f"unexpectedly large SYS_EXIT region ({len(existing)} bytes); refusing to normalize"
        )

    text = text[:start] + CANONICAL_EXIT + text[end:]
    path.write_text(text, encoding="utf-8")
    print(f"Normalized Plasma process-transform exit handler: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
