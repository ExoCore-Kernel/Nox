#!/usr/bin/env python3
"""Apply AF_UNIX semantics and normalize its generated-C integration.

The implementation finalizer deliberately patches a generated unit assembled by
many earlier bring-up stages.  This wrapper performs the implementation first,
then fixes declaration placement and validates the exact poll tail shape before
the later performance transform runs.
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys


def load_module(path: pathlib.Path):
    spec = importlib.util.spec_from_file_location("plasma_unix_sockets_impl", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load AF_UNIX finalizer: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replace_once(text: str, old: str, new: str) -> str:
    found = text.count(old)
    if found != 1:
        raise RuntimeError(
            f"expected exactly one AF_UNIX integration fragment, found {found}: {old[:180]!r}"
        )
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    generated = pathlib.Path(sys.argv[1])
    implementation = pathlib.Path(__file__).with_name("finalize-plasma-unix-sockets.py")
    module = load_module(implementation)
    rc = int(module.main())
    if rc != 0:
        return rc

    text = generated.read_text(encoding="utf-8")

    # getsockopt(SO_PEERCRED) on an unconnected endpoint returns ENOTCONN.
    if "#define LINUX_ENOTCONN" not in text:
        text = replace_once(
            text,
            "#define LINUX_ENOPROTOOPT 92\n",
            "#define LINUX_ENOPROTOOPT 92\n#define LINUX_ENOTCONN   107\n",
        )

    # plasma_close_runtime() now performs socket last-reference reclamation, so
    # its declarations must precede close(), not merely the later socket allocator.
    declarations = (
        "static void plasma_current_ucred(struct plasma_ucred *out);\n"
        "static void plasma_socket_peer_closed(uint16_t object);\n"
        "static void plasma_reclaim_socket_if_unused(uint16_t object);\n"
        "static void plasma_close_cloexec_socket_fds(void);\n\n"
    )
    text = replace_once(
        text,
        declarations + "static int plasma_alloc_socket_object(void) {\n",
        "static int plasma_alloc_socket_object(void) {\n",
    )
    text = replace_once(
        text,
        "static int64_t plasma_close_runtime(int fd) {\n",
        declarations + "static int64_t plasma_close_runtime(int fd) {\n",
    )

    # The implementation inserts POLLHUP after the POLLIN-specific readiness
    # block. Normalize the exact generated tail so there is one, and only one,
    # closing brace for the POLLIN block before the unconditional HUP test.
    malformed_poll_tail = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *s = &plasma_sockets[entry->object];
        if (!s->listening && s->peer < 0) *revents |= POLLHUP;
    }
    }
    return 0;
}
'''
    corrected_poll_tail = r'''    if (entry->type == PLASMA_RT_SOCKET) {
        struct plasma_socket_object *s = &plasma_sockets[entry->object];
        if (!s->listening && s->peer < 0) *revents |= POLLHUP;
    }
    return 0;
}
'''
    text = replace_once(text, malformed_poll_tail, corrected_poll_tail)

    # Structural assertions for the D-Bus blocker itself.  Fail during the
    # generator stage rather than booting another image with fake credentials.
    required = (
        "#define PLASMA_SO_PEERCRED 17",
        "struct plasma_ucred",
        "socket->peer_cred",
        "plasma_socket_getsockopt",
        "[linux:unix] SO_PEERCRED peer pid=",
        "plasma_reclaim_dead_sockets();",
    )
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"AF_UNIX credential output missing marker: {marker}")

    generated.write_text(text, encoding="utf-8")
    print(f"Validated AF_UNIX peer credentials + socket lifetime output: {generated}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
