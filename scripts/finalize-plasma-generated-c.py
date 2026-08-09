#!/usr/bin/env python3
"""Final generated-C cleanup for the Plasma GUI bring-up ABI.

The Plasma build intentionally layers several source transforms over the proven
Bash compatibility unit.  This final pass resolves integration details that only
exist when all of those layers are present together:

* declare the cooperative scheduler lookup before execve's failure helper uses it;
* remove Bash's old SYS_SOCKET -> EAFNOSUPPORT probe stub once AF_UNIX exists;
* avoid taking the address of packed pollfd.revents.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str, label: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one {label} fragment, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    exec_helper = "static int64_t plasma_exec_abandon(const char *reason) {\n"
    text = replace_once(
        text,
        exec_helper,
        "static struct plasma_process *plasma_current_process(void);\n\n" + exec_helper,
        "exec/scheduler declaration",
    )

    # make-bash-shell-compat.py deliberately supplies this before the GUI layer
    # exists.  The runtime IPC transform later installs the real AF_UNIX case.
    text = replace_once(
        text,
        "    case SYS_SOCKET: return -LINUX_EAFNOSUPPORT;\n",
        "",
        "obsolete SYS_SOCKET fallback",
    )

    packed_poll = (
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
        "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n"
    )
    safe_poll = (
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                int16_t runtime_revents = 0;\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &runtime_revents);\n"
        "                pfd.revents = runtime_revents;\n"
        "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n"
    )
    text = replace_once(text, packed_poll, safe_poll, "packed pollfd")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma generated C integration: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
