#!/usr/bin/env python3
"""Final generated-C cleanup for the Plasma GUI bring-up ABI.

The Plasma build intentionally layers several source transforms over the proven
Bash compatibility unit. This final pass resolves integration details that only
exist when all of those layers are present together:

* declare the cooperative scheduler lookup before execve's failure helper uses it;
* remove Bash's old SYS_SOCKET -> EAFNOSUPPORT probe stub once AF_UNIX exists;
* avoid taking the address of packed pollfd.revents, regardless of whether the
  cooperative-I/O transform has already rewritten the surrounding poll block.
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
    declaration = "static struct plasma_process *plasma_current_process(void);\n\n"
    if declaration not in text:
        text = replace_once(
            text,
            exec_helper,
            declaration + exec_helper,
            "exec/scheduler declaration",
        )

    # make-bash-shell-compat.py deliberately supplies this before the GUI layer
    # exists. The runtime IPC transform later installs the real AF_UNIX case.
    socket_fallback = "    case SYS_SOCKET: return -LINUX_EAFNOSUPPORT;\n"
    socket_count = text.count(socket_fallback)
    if socket_count == 1:
        text = text.replace(socket_fallback, "", 1)
    elif socket_count != 0:
        raise RuntimeError(
            f"expected zero or one obsolete SYS_SOCKET fallback, found {socket_count}"
        )

    # finalize-plasma-cooperative-io.py runs immediately before this pass and
    # changes the poll branch from wants_input to runtime_wait. Accept both forms
    # so this cleanup remains order-safe and idempotent.
    packed_variants = [
        (
            "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
            "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
            "                runtime_wait = true;\n"
            "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
            "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
            "                int16_t runtime_revents = 0;\n"
            "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &runtime_revents);\n"
            "                pfd.revents = runtime_revents;\n"
            "                runtime_wait = true;\n"
            "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
        ),
        (
            "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
            "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
            "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
            "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
            "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
            "                int16_t runtime_revents = 0;\n"
            "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &runtime_revents);\n"
            "                pfd.revents = runtime_revents;\n"
            "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
            "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
        ),
    ]

    replacements = 0
    for old, new in packed_variants:
        count = text.count(old)
        if count > 1:
            raise RuntimeError(f"expected at most one packed pollfd fragment, found {count}")
        if count == 1:
            text = text.replace(old, new, 1)
            replacements += 1

    safe_marker = (
        "                int16_t runtime_revents = 0;\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &runtime_revents);\n"
        "                pfd.revents = runtime_revents;\n"
    )
    if replacements > 1:
        raise RuntimeError("multiple packed pollfd variants matched")
    if replacements == 0 and safe_marker not in text:
        raise RuntimeError("no recognized runtime pollfd layout found")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma generated C integration: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
