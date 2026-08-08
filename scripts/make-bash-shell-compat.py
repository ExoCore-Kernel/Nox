#!/usr/bin/env python3
"""Generate Twilight's Bash shell compatibility unit from the ash bring-up unit.

This does not modify GNU Bash. It only reuses Twilight's userspace/TTY ABI
shim while changing the process identity exposed to userspace from /bin/sh to
/bin/bash and adding Linux ABI calls used by the static glibc Bash build.
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

    # GNU readline uses select/pselect to wait for terminal input. The original
    # ash bring-up shim returned ENOSYS for both, which Bash can interpret as an
    # unusable/ended input stream. Implement the Linux fd_set behavior needed by
    # our early single-TTY process. Finite timeouts are currently treated as
    # nonblocking polls; an infinite read wait blocks on the IRQ/serial TTY.
    select_helpers = r'''
#define BASH_FDSET_BYTES 128u

static bool bash_fdset_test(const uint8_t set[BASH_FDSET_BYTES], unsigned int fd) {
    if (fd >= BASH_FDSET_BYTES * 8u) return false;
    return (set[fd >> 3u] & (uint8_t)(1u << (fd & 7u))) != 0;
}

static void bash_fdset_set(uint8_t set[BASH_FDSET_BYTES], unsigned int fd) {
    if (fd >= BASH_FDSET_BYTES * 8u) return;
    set[fd >> 3u] |= (uint8_t)(1u << (fd & 7u));
}

static bool bash_fdset_has_unsupported(const uint8_t set[BASH_FDSET_BYTES],
                                       uint64_t nfds) {
    if (nfds > BASH_FDSET_BYTES * 8u) nfds = BASH_FDSET_BYTES * 8u;
    for (uint64_t fd = 10; fd < nfds; ++fd)
        if (bash_fdset_test(set, (unsigned int)fd)) return true;
    return false;
}

static int64_t bash_select_tty(uint64_t nfds,
                               uint64_t read_address,
                               uint64_t write_address,
                               uint64_t except_address,
                               uint64_t timeout_address) {
    if (nfds > BASH_FDSET_BYTES * 8u) return -LINUX_EINVAL;

    uint8_t requested_read[BASH_FDSET_BYTES];
    uint8_t requested_write[BASH_FDSET_BYTES];
    uint8_t requested_except[BASH_FDSET_BYTES];
    uint8_t result_read[BASH_FDSET_BYTES];
    uint8_t result_write[BASH_FDSET_BYTES];
    uint8_t result_except[BASH_FDSET_BYTES];
    bytes_zero(requested_read, sizeof(requested_read));
    bytes_zero(requested_write, sizeof(requested_write));
    bytes_zero(requested_except, sizeof(requested_except));

    if (read_address != 0 &&
        !user_copy_in(requested_read, read_address, sizeof(requested_read)))
        return -LINUX_EFAULT;
    if (write_address != 0 &&
        !user_copy_in(requested_write, write_address, sizeof(requested_write)))
        return -LINUX_EFAULT;
    if (except_address != 0 &&
        !user_copy_in(requested_except, except_address, sizeof(requested_except)))
        return -LINUX_EFAULT;

    if (bash_fdset_has_unsupported(requested_read, nfds) ||
        bash_fdset_has_unsupported(requested_write, nfds) ||
        bash_fdset_has_unsupported(requested_except, nfds))
        return -LINUX_EBADF;

    const bool finite_timeout = timeout_address != 0;

    for (;;) {
        bytes_zero(result_read, sizeof(result_read));
        bytes_zero(result_write, sizeof(result_write));
        bytes_zero(result_except, sizeof(result_except));
        bool ready_fd[10] = { false, false, false, false, false,
                              false, false, false, false, false };

        const bool input_ready = tty_input_available();
        for (unsigned int fd = 0; fd < 10u && fd < nfds; ++fd) {
            if (bash_fdset_test(requested_read, fd) &&
                (fd == 0u || fd == 3u) && input_ready) {
                bash_fdset_set(result_read, fd);
                ready_fd[fd] = true;
            }
            if (bash_fdset_test(requested_write, fd) && fd_is_tty((int)fd)) {
                bash_fdset_set(result_write, fd);
                ready_fd[fd] = true;
            }
        }

        int64_t ready = 0;
        for (unsigned int fd = 0; fd < 10u && fd < nfds; ++fd)
            if (ready_fd[fd]) ++ready;

        if (ready != 0 || finite_timeout) {
            if (read_address != 0 &&
                !user_copy_out(read_address, result_read, sizeof(result_read)))
                return -LINUX_EFAULT;
            if (write_address != 0 &&
                !user_copy_out(write_address, result_write, sizeof(result_write)))
                return -LINUX_EFAULT;
            if (except_address != 0 &&
                !user_copy_out(except_address, result_except, sizeof(result_except)))
                return -LINUX_EFAULT;
            return ready;
        }

        bool wants_input = false;
        if (nfds > 0 && bash_fdset_test(requested_read, 0)) wants_input = true;
        if (nfds > 3 && bash_fdset_test(requested_read, 3)) wants_input = true;
        if (!wants_input) {
            if (read_address != 0)
                (void)user_copy_out(read_address, result_read, sizeof(result_read));
            if (write_address != 0)
                (void)user_copy_out(write_address, result_write, sizeof(result_write));
            if (except_address != 0)
                (void)user_copy_out(except_address, result_except, sizeof(result_except));
            return 0;
        }
        tty_wait_for_input();
    }
}

'''
    text = require_replace(text,
                           'static int64_t shell_dispatch(uint64_t number,\n',
                           select_helpers + 'static int64_t shell_dispatch(uint64_t number,\n')

    old_select = '''    case SYS_SELECT:
    case SYS_PSELECT6:
        /* BusyBox's line editor can use poll/read on this terminal. Returning
         * ENOSYS here lets libc fall back rather than fabricating fd_sets. */
        return -LINUX_ENOSYS;
'''
    new_select = '''    case SYS_SELECT:
        return bash_select_tty(a1, a2, a3, a4, a5);
    case SYS_PSELECT6:
        /* pselect6's sixth argument describes a temporary signal mask. Signal
         * delivery is not implemented yet, so the fd readiness semantics are
         * identical to select for this single foreground process. */
        return bash_select_tty(a1, a2, a3, a4, a5);
'''
    text = require_replace(text, old_select, new_select)

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
