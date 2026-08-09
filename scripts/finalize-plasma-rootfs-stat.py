#!/usr/bin/env python3
"""Give Plasma rootfs files stable st_dev/st_ino identities.

musl's dynamic linker uses the (st_dev, st_ino) pair returned by fstat() to
recognize a DSO it has already loaded.  The original shell compatibility stat
shim zeroed both fields for every CPIO file, so after the first shared library
musl incorrectly treated all subsequent libraries as aliases of that same DSO.

The CPIO-facing rootfs node does not currently expose c_ino, so derive a stable
64-bit inode from the resolved node name.  Because rootfs_lookup_follow() returns
the final target node, different symlink spellings of the same file naturally
produce the same identity.  A fixed nonzero device number identifies the
read-only Twilight initramfs filesystem.
"""
from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"expected exactly one rootfs stat fragment, found {count}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    old = r'''static int64_t fill_rootfs_stat(uint64_t address, const struct rootfs_node *node) {
    if (node == 0) return -LINUX_EINVAL;
    int64_t rc = fill_stat(address, node->mode);
    if (rc != 0) return rc;
    if (!user_store_u64(address + 48, node->size)) return -LINUX_EFAULT; /* st_size */
    if (!user_store_u64(address + 56, 4096)) return -LINUX_EFAULT;      /* st_blksize */
    const uint64_t blocks = (node->size + 511u) / 512u;
    if (!user_store_u64(address + 64, blocks)) return -LINUX_EFAULT;    /* st_blocks */
    return 0;
}
'''

    new = r'''static uint64_t plasma_rootfs_inode(const struct rootfs_node *node) {
    /* FNV-1a over the resolved archive name: deterministic, nonzero, and
     * sufficient for Linux userspace's file-identity comparisons. */
    uint64_t hash = 1469598103934665603ull;
    if (node != 0 && node->name != 0) {
        const char *p = node->name;
        while (*p != '\0') {
            hash ^= (uint8_t)*p++;
            hash *= 1099511628211ull;
        }
    }
    return hash != 0 ? hash : 1ull;
}

static int64_t fill_rootfs_stat(uint64_t address, const struct rootfs_node *node) {
    if (node == 0) return -LINUX_EINVAL;
    int64_t rc = fill_stat(address, node->mode);
    if (rc != 0) return rc;
    /* x86_64 Linux struct stat: st_dev @ 0, st_ino @ 8. musl's loader uses
     * this pair to decide whether two opened DSOs are the same file. */
    if (!user_store_u64(address + 0, 0x4e4f5801ull)) return -LINUX_EFAULT; /* st_dev: NOX + fs id */
    if (!user_store_u64(address + 8, plasma_rootfs_inode(node))) return -LINUX_EFAULT; /* st_ino */
    if (!user_store_u64(address + 48, node->size)) return -LINUX_EFAULT; /* st_size */
    if (!user_store_u64(address + 56, 4096)) return -LINUX_EFAULT;      /* st_blksize */
    const uint64_t blocks = (node->size + 511u) / 512u;
    if (!user_store_u64(address + 64, blocks)) return -LINUX_EFAULT;    /* st_blocks */
    return 0;
}
'''

    text = replace_once(text, old, new)
    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma rootfs stat identity for musl DSOs: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
