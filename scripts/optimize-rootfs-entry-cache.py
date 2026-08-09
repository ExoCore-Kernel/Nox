#!/usr/bin/env python3
"""Patch Twilight rootfs_entry_at() with a sequential CPIO iterator cache.

Xorg's getdents64 path walks rootfs entries in increasing index order. The old
rootfs_entry_at(index) restarted at archive offset 0 for every index, turning a
51k-entry directory scan into O(n^2) CPIO parsing. This keeps the public API the
same while caching the next archive offset, making sequential scans O(n).

The patch is idempotent so the Plasma bring-up script can run it every time.
"""

from __future__ import annotations

import pathlib
import sys

MARKER = "rootfs_entry_cache_next_offset"


def main() -> int:
    path = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "twilight/kernel/rootfs.c")
    text = path.read_text(encoding="utf-8")

    if MARKER in text:
        print(f"Rootfs sequential entry cache already enabled: {path}")
        return 0

    globals_old = """static size_t rootfs_entries;\nstatic bool rootfs_ready;\n"""
    globals_new = """static size_t rootfs_entries;\nstatic bool rootfs_ready;\n\n/* Sequential rootfs_entry_at() accelerator. getdents64 walks increasing\n * indices, so retaining the next CPIO offset avoids reparsing the archive\n * from byte zero for every entry. Random/backward access safely restarts. */\nstatic size_t rootfs_entry_cache_index;\nstatic size_t rootfs_entry_cache_next_offset;\nstatic bool rootfs_entry_cache_valid;\n"""
    if globals_old not in text:
        raise RuntimeError("rootfs globals fragment not found")
    text = text.replace(globals_old, globals_new, 1)

    init_old = """    rootfs_entries = 0;\n    rootfs_ready = false;\n\n    if (archive == 0 || size < CPIO_NEWC_HEADER_SIZE) return false;\n"""
    init_new = """    rootfs_entries = 0;\n    rootfs_ready = false;\n    rootfs_entry_cache_index = 0;\n    rootfs_entry_cache_next_offset = 0;\n    rootfs_entry_cache_valid = false;\n\n    if (archive == 0 || size < CPIO_NEWC_HEADER_SIZE) return false;\n"""
    if init_old not in text:
        raise RuntimeError("rootfs_init fragment not found")
    text = text.replace(init_old, init_new, 1)

    old_fn = """bool rootfs_entry_at(size_t index, struct rootfs_node *out) {\n    if (!rootfs_ready || out == 0 || index >= rootfs_entries) return false;\n\n    size_t offset = 0;\n    size_t current = 0;\n    while (offset < rootfs_archive_size) {\n        struct parsed_entry entry;\n        if (!parse_entry(offset, &entry)) return false;\n        if (string_equal(entry.name, \"TRAILER!!!\")) return false;\n        if (current == index) {\n            out->name = entry.name;\n            out->data = entry.data;\n            out->size = entry.size;\n            out->mode = entry.mode;\n            return true;\n        }\n        ++current;\n        offset = entry.next_offset;\n    }\n    return false;\n}\n"""

    new_fn = """bool rootfs_entry_at(size_t index, struct rootfs_node *out) {\n    if (!rootfs_ready || out == 0 || index >= rootfs_entries) return false;\n\n    size_t offset;\n    size_t current;\n\n    /* Fast path: the previous call asked for index-1. This is exactly the\n     * access pattern used by getdents64, so parse only the next CPIO header. */\n    if (rootfs_entry_cache_valid && index == rootfs_entry_cache_index) {\n        offset = rootfs_entry_cache_next_offset;\n        current = index;\n    } else {\n        offset = 0;\n        current = 0;\n    }\n\n    while (offset < rootfs_archive_size) {\n        struct parsed_entry entry;\n        if (!parse_entry(offset, &entry)) {\n            rootfs_entry_cache_valid = false;\n            return false;\n        }\n        if (string_equal(entry.name, \"TRAILER!!!\")) {\n            rootfs_entry_cache_valid = false;\n            return false;\n        }\n        if (current == index) {\n            out->name = entry.name;\n            out->data = entry.data;\n            out->size = entry.size;\n            out->mode = entry.mode;\n\n            rootfs_entry_cache_index = index + 1u;\n            rootfs_entry_cache_next_offset = entry.next_offset;\n            rootfs_entry_cache_valid = true;\n            return true;\n        }\n        ++current;\n        offset = entry.next_offset;\n    }\n\n    rootfs_entry_cache_valid = false;\n    return false;\n}\n"""
    if old_fn not in text:
        raise RuntimeError("rootfs_entry_at fragment not found")
    text = text.replace(old_fn, new_fn, 1)

    path.write_text(text, encoding="utf-8")
    print(f"Enabled O(n) sequential CPIO enumeration cache: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
