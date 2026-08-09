#!/usr/bin/env python3
"""Provide the interval-timer ABI Xlib uses while opening the X11 display.

startplasma-x11 calls kCheckRunning(), which opens DISPLAY through libX11.
Twilight's AF_UNIX connect already succeeds, but libX11 calls setitimer(2) while
establishing the connection.  The local bring-up transport does not need real
SIGALRM delivery yet, so this finalizer supplies inert interval timers and then
runs the X11 socket-fcntl/TX finalizer that corrects descriptor status semantics
and traces the first setup packet.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected X11 timer fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "#define SYS_NANOSLEEP       35ull\n",
        "#define SYS_NANOSLEEP       35ull\n"
        "#define SYS_GETITIMER       36ull\n"
        "#define SYS_SETITIMER       38ull\n",
    )

    # struct itimerval on x86_64 is two struct timeval values: four 64-bit
    # fields total (interval sec/usec + current-value sec/usec).
    timer_cases = r'''    case SYS_GETITIMER:
        if (a1 > 2u) return -LINUX_EINVAL;
        if (a2 != 0 && !user_zero(a2, 32u)) return -LINUX_EFAULT;
        return 0;
    case SYS_SETITIMER:
        if (a1 > 2u) return -LINUX_EINVAL;
        /* Xlib uses this as a connection timeout guard.  The current local
         * AF_UNIX transport is cooperative and immediate, so accept the timer
         * without scheduling SIGALRM.  Linux still reports the previous timer
         * through old_value when requested. */
        if (a3 != 0 && !user_zero(a3, 32u)) return -LINUX_EFAULT;
        serial_write("[linux:x11] setitimer accepted as inert X11 timeout\n");
        return 0;
'''

    text = rep(
        text,
        "    case SYS_NANOSLEEP:\n"
        "        if (a2 != 0 && !user_zero(a2, 16)) return -LINUX_EFAULT;\n"
        "        return 0;\n",
        "    case SYS_NANOSLEEP:\n"
        "        if (a2 != 0 && !user_zero(a2, 16)) return -LINUX_EFAULT;\n"
        "        return 0;\n" + timer_cases,
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized inert getitimer/setitimer ABI for XOpenDisplay: {path}")

    # Keep the shell runner stable while the X11 compatibility layer is being
    # iterated: the sibling finalizer owns runtime-socket fcntl semantics and TX
    # diagnostics, and is intentionally applied after the timer cases above.
    sibling = pathlib.Path(__file__).with_name("finalize-plasma-x11-socket-fcntl.py")
    subprocess.run([sys.executable, str(sibling), str(path)], check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
