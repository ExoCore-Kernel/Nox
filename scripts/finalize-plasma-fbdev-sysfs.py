#!/usr/bin/env python3
"""Provide the minimal synthetic sysfs identity Xorg fbdev expects.

Xorg's fbdevhw helper opens /dev/fb0 and then readlinks the corresponding
/sys/class/graphics path to decide whether the framebuffer belongs to the PCI
path or the generic platform path. Twilight has no sysfs yet, so a successful
/dev/fb0 open was being thrown away and xf86-video-fbdev reported
"No devices detected". Keep the real /dev/fb0 ABI and synthesize only the two
readlink targets used by fbdevhw during this early Plasma bring-up stage.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected fbdev sysfs fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    old_readlink = '''    case SYS_READLINK: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    new_readlink = '''    case SYS_READLINK: {
        char path[128];
        if (!copy_user_string(a1, path, sizeof(path))) return -LINUX_EFAULT;
        if (string_equal(path, "/sys/class/graphics/fb0/device/subsystem")) {
            const char target[] = "../../../bus/platform";
            size_t length = sizeof(target) - 1u;
            if (length > a3) length = (size_t)a3;
            serial_write("[linux:fbdev] synthetic sysfs subsystem -> bus/platform\\n");
            return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
        }
        if (string_equal(path, "/sys/class/graphics/fb0")) {
            const char target[] = "../../devices/platform/twilight-framebuffer.0/graphics/fb0";
            size_t length = sizeof(target) - 1u;
            if (length > a3) length = (size_t)a3;
            serial_write("[linux:fbdev] synthetic sysfs fb0 platform link\\n");
            return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
        }
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a3) length = (size_t)a3;
        return user_copy_out(a2, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    text = replace_once(text, old_readlink, new_readlink)

    old_readlinkat = '''    case SYS_READLINKAT: {
        char path[128];
        if (!copy_user_string(a2, path, sizeof(path))) return -LINUX_EFAULT;
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a4) length = (size_t)a4;
        return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    new_readlinkat = '''    case SYS_READLINKAT: {
        char path[128];
        if (!copy_user_string(a2, path, sizeof(path))) return -LINUX_EFAULT;
        if (string_equal(path, "/sys/class/graphics/fb0/device/subsystem")) {
            const char target[] = "../../../bus/platform";
            size_t length = sizeof(target) - 1u;
            if (length > a4) length = (size_t)a4;
            serial_write("[linux:fbdev] synthetic sysfs subsystem -> bus/platform\\n");
            return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
        }
        if (string_equal(path, "/sys/class/graphics/fb0")) {
            const char target[] = "../../devices/platform/twilight-framebuffer.0/graphics/fb0";
            size_t length = sizeof(target) - 1u;
            if (length > a4) length = (size_t)a4;
            serial_write("[linux:fbdev] synthetic sysfs fb0 platform link\\n");
            return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
        }
        if (!string_equal(path, "/proc/self/exe")) return -LINUX_ENOENT;
        const char target[] = "/bin/bash";
        size_t length = sizeof(target) - 1u;
        if (length > a4) length = (size_t)a4;
        return user_copy_out(a3, target, length) ? (int64_t)length : -LINUX_EFAULT;
    }
'''
    text = replace_once(text, old_readlinkat, new_readlinkat)

    path.write_text(text, encoding="utf-8")
    print(f"Added synthetic Xorg fbdev sysfs identity: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
