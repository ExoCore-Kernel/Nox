#!/usr/bin/env python3
"""Finish filesystem semantics needed by Xorg's xkbcomp helper.

The early Plasma ABI intentionally has a read-only CPIO root plus a tiny
writable runtime VFS. xkbcomp needs two things that were still missing:

* chdir("/usr/share/X11/xkb") followed by relative opens of rules/symbols/etc.
* a writable /var/lib/xkb/server-*.xkm output path.

This finalizer adds cwd-aware path resolution for the common path syscalls and
extends the writable bring-up VFS only to /var/lib/xkb. It does not make the
whole CPIO writable.
"""
from __future__ import annotations

import pathlib
import re
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected XKB path fragment not found: {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Make the XKB cache directory a small writable runtime node. /var and
    # /var/lib remain supplied by the read-only Alpine CPIO; only this leaf is
    # overlaid by Twilight's bring-up VFS.
    text = rep(
        text,
        '    const char *dirs[] = { "/tmp", "/tmp/.X11-unix", "/tmp/runtime-root", "/run", "/run/user", "/run/user/0" };\n',
        '    const char *dirs[] = { "/tmp", "/tmp/.X11-unix", "/tmp/runtime-root", "/run", "/run/user", "/run/user/0", "/var/lib/xkb" };\n',
    )

    old_runtime_path = r'''static bool plasma_runtime_path(const char *path) {
    return path != 0 &&
           ((path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
             (path[4] == '\0' || path[4] == '/')) ||
            (path[0] == '/' && path[1] == 'r' && path[2] == 'u' && path[3] == 'n' &&
             (path[4] == '\0' || path[4] == '/')));
}
'''
    new_runtime_path = r'''static bool plasma_runtime_path(const char *path) {
    if (path == 0) return false;
    if (path[0] == '/' && path[1] == 't' && path[2] == 'm' && path[3] == 'p' &&
        (path[4] == '\0' || path[4] == '/')) return true;
    if (path[0] == '/' && path[1] == 'r' && path[2] == 'u' && path[3] == 'n' &&
        (path[4] == '\0' || path[4] == '/')) return true;
    const char xkb[] = "/var/lib/xkb";
    size_t i = 0;
    while (xkb[i] != '\0' && path[i] == xkb[i]) ++i;
    return xkb[i] == '\0' && (path[i] == '\0' || path[i] == '/');
}
'''
    text = rep(text, old_runtime_path, new_runtime_path)

    # Resolve relative Linux paths against the process cwd. xkbcomp deliberately
    # chdirs into its -R directory and then opens files such as rules/evdev and
    # symbols/pc by relative name, so merely making chdir() return success would
    # still leave it unable to read the XKB database.
    helper_anchor = "static bool path_is_known(const char *path) {\n"
    helper = r'''static bool plasma_resolve_path(const char *input, char *out, size_t capacity) {
    if (input == 0 || out == 0 || capacity < 2u || input[0] == '\0') return false;

    char raw[256];
    size_t raw_n = 0;
    if (input[0] != '/') {
        const size_t cwd_n = string_length(current_directory);
        if (cwd_n == 0 || cwd_n + 2u >= sizeof(raw)) return false;
        bytes_copy(raw, current_directory, cwd_n);
        raw_n = cwd_n;
        if (raw_n == 0 || raw[raw_n - 1u] != '/') raw[raw_n++] = '/';
    }
    const size_t input_n = string_length(input);
    if (raw_n + input_n + 1u > sizeof(raw)) return false;
    bytes_copy(raw + raw_n, input, input_n + 1u);

    /* Canonicalize //, /./ and /../ without needing libc. */
    size_t r = 0, w = 0;
    out[w++] = '/';
    while (raw[r] != '\0') {
        while (raw[r] == '/') ++r;
        if (raw[r] == '\0') break;
        const size_t seg = r;
        while (raw[r] != '\0' && raw[r] != '/') ++r;
        const size_t n = r - seg;
        if (n == 1u && raw[seg] == '.') continue;
        if (n == 2u && raw[seg] == '.' && raw[seg + 1u] == '.') {
            if (w > 1u) {
                if (out[w - 1u] == '/') --w;
                while (w > 1u && out[w - 1u] != '/') --w;
            }
            continue;
        }
        if (w > 1u && out[w - 1u] != '/') {
            if (w + 1u >= capacity) return false;
            out[w++] = '/';
        }
        if (w + n + 1u > capacity) return false;
        bytes_copy(out + w, raw + seg, n);
        w += n;
    }
    if (w > 1u && out[w - 1u] == '/') --w;
    out[w] = '\0';
    return true;
}

'''
    text = rep(text, helper_anchor, helper + helper_anchor)

    stat_old = '''static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
'''
    stat_new = '''static int64_t stat_path(uint64_t path_address, uint64_t stat_address) {
    char path[256], resolved[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (!plasma_resolve_path(path, resolved, sizeof(resolved))) return -LINUX_ENOENT;
    bytes_copy(path, resolved, string_length(resolved) + 1u);
'''
    text = rep(text, stat_old, stat_new)

    open_old = '''static int64_t sys_open_path(uint64_t path_address, uint32_t flags, uint32_t mode) {
    char path[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
'''
    open_new = '''static int64_t sys_open_path(uint64_t path_address, uint32_t flags, uint32_t mode) {
    char path[256], resolved[256];
    if (!copy_user_string(path_address, path, sizeof(path))) return -LINUX_EFAULT;
    if (!plasma_resolve_path(path, resolved, sizeof(resolved))) return -LINUX_ENOENT;
    bytes_copy(path, resolved, string_length(resolved) + 1u);
'''
    text = rep(text, open_old, open_new)

    # access(2) has a dedicated fbdev special case from the framebuffer finalizer.
    access_old = '''    case SYS_ACCESS: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (string_equal(path, "/dev/fb0")) return plasma_fb_available() ? 0 : -LINUX_ENOENT;
        return path_is_known(path) ? 0 : -LINUX_ENOENT;
    }
'''
    access_new = '''    case SYS_ACCESS: {
        char path[256], resolved[256];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!plasma_resolve_path(path, resolved, sizeof(resolved))) return -LINUX_ENOENT;
        if (string_equal(resolved, "/dev/fb0")) return plasma_fb_available() ? 0 : -LINUX_ENOENT;
        if (plasma_runtime_path(resolved) && plasma_find_tmp(resolved) >= 0) return 0;
        return path_is_known(resolved) ? 0 : -LINUX_ENOENT;
    }
'''
    text = rep(text, access_old, access_new)

    # Several earlier Plasma finalizers can legitimately add logic inside the
    # CHDIR case. Matching its old exact body made this transform brittle. Replace
    # the whole switch case structurally, stopping at the following syscall case.
    # Keep this a raw Python string, and use a callable re.sub replacement below:
    # otherwise Python/re.sub would turn the C "\\n" escape into a literal newline
    # inside the generated C string and break compilation.
    chdir_new = r'''    case SYS_CHDIR: {
        char path[256], resolved[256];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!plasma_resolve_path(path, resolved, sizeof(resolved))) return -LINUX_ENOENT;

        bool directory = false;
        const int runtime_index = plasma_runtime_path(resolved) ? plasma_find_tmp(resolved) : -1;
        if (runtime_index >= 0 && plasma_tmp_nodes[runtime_index].directory) {
            directory = true;
        } else {
            struct rootfs_node node;
            if (rootfs_available() && rootfs_lookup_follow(resolved, &node) &&
                (node.mode & 0170000u) == S_IFDIR)
                directory = true;
        }
        if (!directory) return -LINUX_ENOENT;

        const size_t n = string_length(resolved);
        if (n + 1u > sizeof(current_directory)) return -LINUX_ENOMEM;
        bytes_copy(current_directory, resolved, n + 1u);
        serial_write("[linux:xkb] chdir -> "); serial_write(current_directory); serial_write("\n");
        return 0;
    }
'''
    chdir_pattern = re.compile(
        r"(?ms)^    case SYS_CHDIR: \{\n.*?^    \}\n(?=    case SYS_[A-Z0-9_]+:)"
    )
    matches = list(chdir_pattern.finditer(text))
    if len(matches) != 1:
        raise RuntimeError(f"expected exactly one SYS_CHDIR switch case, found {len(matches)}")
    text = chdir_pattern.sub(lambda _match: chdir_new, text, count=1)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized XKB cwd + writable cache paths: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
