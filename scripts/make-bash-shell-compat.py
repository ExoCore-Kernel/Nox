#!/usr/bin/env python3
"""Generate Twilight's Bash shell compatibility unit from the ash bring-up unit.

This does not modify GNU Bash. It only reuses Twilight's userspace/TTY ABI
shim while changing the process identity exposed to userspace from /bin/sh to
/bin/bash and adding a few Linux ABI calls used by the static glibc Bash build.
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

    # Process identity: this is genuinely Bash invoked as /bin/bash -i.
    text = require_replace(text, 'const char argv0[] = "/bin/sh";',
                           'const char argv0[] = "/bin/bash";')
    text = require_replace(text, 'const char target[] = "/bin/busybox";',
                           'const char target[] = "/bin/bash";')

    # Debian bash-static is glibc based and touches a few Linux calls that the
    # BusyBox/musl shell did not need during startup. Keep these in the Bash
    # generated compatibility unit until the common Linux UAPI layer grows.
    text = require_replace(
        text,
        '#define SYS_GETPID          39ull\n',
        '#define SYS_GETPID          39ull\n'
        '#define SYS_SOCKET          41ull\n',
    )
    text = require_replace(
        text,
        '#define SYS_GETTID         186ull\n',
        '#define SYS_GETTID         186ull\n'
        '#define SYS_TIME           201ull\n',
    )
    text = require_replace(
        text,
        '#define SYS_NEWFSTATAT     262ull\n',
        '#define SYS_NEWFSTATAT     262ull\n'
        '#define SYS_READLINKAT     267ull\n',
    )
    text = require_replace(
        text,
        '#define LINUX_ENOSYS     38\n',
        '#define LINUX_ENOSYS     38\n'
        '#define LINUX_EAFNOSUPPORT 97\n',
    )

    # time(2): until Twilight has an RTC/wall-clock source, return a valid epoch
    # value of zero rather than ENOSYS. This is intentionally not pretending to
    # provide real wall time yet.
    text = require_replace(
        text,
        '    case SYS_GETPID:\n    case SYS_GETTID: return 1;\n',
        '    case SYS_GETPID:\n    case SYS_GETTID: return 1;\n'
        '    case SYS_TIME:\n'
        '        if (a1 != 0 && !user_store_u64(a1, 0)) return -LINUX_EFAULT;\n'
        '        return 0;\n',
    )

    # Bash/glibc probes sockets during startup. Userspace sockets are not wired
    # to Twilight networking yet, so report the precise Linux condition rather
    # than an unknown syscall. Real socket support comes with the driver-backed
    # networking milestone.
    text = require_replace(
        text,
        '    case SYS_MADVISE: return 0;\n',
        '    case SYS_MADVISE: return 0;\n'
        '    case SYS_SOCKET: return -LINUX_EAFNOSUPPORT;\n',
    )

    # readlinkat(AT_FDCWD, "/proc/self/exe", ...), used by static glibc/Bash.
    # For absolute paths the dirfd is irrelevant, matching Linux semantics.
    readlink_case = '''    case SYS_READLINK: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    readlink_with_at = readlink_case + '''    case SYS_READLINKAT: {
        char path[128];
        if (!copy_user_string(a2, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a4) length = (size_t)a4;
        return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    text = require_replace(text, readlink_case, readlink_with_at)

    # Trace the true process-exit boundary. If a later run prints this before a
    # crash, the fault is in Twilight's unwind path; if it never prints, Bash
    # faulted during userspace exit handlers before invoking exit/exit_group.
    text = require_replace(
        text,
        '    case SYS_EXIT:\n'
        '    case SYS_EXIT_GROUP:\n'
        '        shell_exit_status = (int)(a1 & 0xffu);\n',
        '    case SYS_EXIT:\n'
        '    case SYS_EXIT_GROUP:\n'
        '        serial_write("[linux:bash] exit syscall observed, status=");\n'
        '        serial_u64(a1 & 0xffu);\n'
        '        serial_write("\\n");\n'
        '        shell_exit_status = (int)(a1 & 0xffu);\n',
    )

    # Diagnostics only. Keep C symbol names and TWILIGHT_BUSYBOX_SELF_TEST
    # intact because the common SYSCALL router ABI is intentionally shared.
    text = text.replace("busybox-shell", "bash-shell")
    text = text.replace("linux:ash", "linux:bash")
    text = text.replace("UNMODIFIED BusyBox /bin/sh", "GNU Bash /bin/bash")
    text = text.replace("BusyBox ELF loader rejected shell instance",
                        "GNU Bash ELF loader rejected shell instance")
    text = text.replace("official unmodified BusyBox ash interactive session",
                        "GNU Bash interactive session")
    text = text.replace("Twilight BusyBox shell", "Twilight GNU Bash shell")

    # The BusyBox source used a fixed byte count. Changing the banner text to
    # "GNU Bash" made that count stale and caused the prompt to run into it.
    bash_banner = (
        '    tty_write("\\nTwilight GNU Bash shell — built-ins work now; '
        "type 'exit' to return to kernel.\\n\", 79);"
    )
    if bash_banner in text:
        text = text.replace(
            bash_banner,
            '    static const char bash_banner[] = "\\nTwilight GNU Bash shell — built-ins work now; '
            "type 'exit' to return to kernel.\\n\";\n"
            '    tty_write(bash_banner, sizeof(bash_banner) - 1u);',
        )

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
