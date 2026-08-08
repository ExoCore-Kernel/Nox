#!/usr/bin/env python3
"""Generate Twilight's Bash shell compatibility unit from the ash bring-up unit.

This does not modify GNU Bash.  It only reuses Twilight's userspace/TTY ABI
shim while changing the process identity exposed to userspace from /bin/sh to
/bin/bash and making diagnostics say Bash instead of BusyBox/ash.
"""

from __future__ import annotations

import pathlib
import sys


def require_replace(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected source text not found: {old!r}")
    return text.replace(old, new)


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} INPUT OUTPUT", file=sys.stderr)
        return 2

    source = pathlib.Path(sys.argv[1])
    output = pathlib.Path(sys.argv[2])
    text = source.read_text(encoding="utf-8")

    text = require_replace(text, 'const char argv0[] = "/bin/sh";',
                           'const char argv0[] = "/bin/bash";')
    text = require_replace(text, 'const char target[] = "/bin/busybox";',
                           'const char target[] = "/bin/bash";')

    # Diagnostics only. Keep C symbol names and TWILIGHT_BUSYBOX_SELF_TEST
    # intact because the common SYSCALL router ABI is intentionally shared.
    text = text.replace("busybox-shell", "bash-shell")
    text = text.replace("UNMODIFIED BusyBox /bin/sh", "GNU Bash /bin/bash")
    text = text.replace("BusyBox ELF loader rejected shell instance",
                        "GNU Bash ELF loader rejected shell instance")
    text = text.replace("official unmodified BusyBox ash interactive session",
                        "GNU Bash interactive session")
    text = text.replace("Twilight BusyBox shell", "Twilight GNU Bash shell")

    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    print(f"Generated Bash compatibility unit: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
