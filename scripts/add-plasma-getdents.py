#!/usr/bin/env python3
"""Add read-only getdents64 directory enumeration for the CPIO rootfs."""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected getdents source fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    code = r'''
#define PLASMA_DT_DIR 4u
#define PLASMA_DT_REG 8u
#define PLASMA_DT_LNK 10u
#define PLASMA_MODE_MASK 0170000u
#define PLASMA_MODE_DIR  0040000u
#define PLASMA_MODE_LNK  0120000u
#define PLASMA_GETDENTS_RUNAWAY_LIMIT 100000u
#define PLASMA_GETDENTS_TRACE_STRIDE 4096u

struct __attribute__((packed)) plasma_linux_dirent64_head {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
};

static const char *plasma_archive_name(const char *name) {
    if (name == 0) return "";
    while (*name == '/') ++name;
    while (name[0] == '.' && name[1] == '/') name += 2;
    return name;
}

static bool plasma_immediate_child(const char *directory,
                                   const char *entry,
                                   const char **child_name) {
    directory = plasma_archive_name(directory);
    entry = plasma_archive_name(entry);
    if (child_name == 0 || entry[0] == '\0') return false;

    if ((directory[0] == '.' && directory[1] == '\0') || directory[0] == '\0') {
        if (entry[0] == '.' && entry[1] == '\0') return false;
        for (const char *p = entry; *p != '\0'; ++p)
            if (*p == '/') return false;
        *child_name = entry;
        return true;
    }

    size_t n = 0;
    while (directory[n] != '\0') {
        if (entry[n] != directory[n]) return false;
        ++n;
    }
    if (entry[n] != '/') return false;
    ++n;
    if (entry[n] == '\0') return false;
    const char *child = entry + n;
    for (const char *p = child; *p != '\0'; ++p)
        if (*p == '/') return false;
    *child_name = child;
    return true;
}

static uint8_t plasma_dirent_type(uint32_t mode) {
    switch (mode & PLASMA_MODE_MASK) {
    case PLASMA_MODE_DIR: return PLASMA_DT_DIR;
    case PLASMA_MODE_LNK: return PLASMA_DT_LNK;
    default: return PLASMA_DT_REG;
    }
}

static int64_t plasma_emit_dirent(uint64_t buffer_address,
                                  uint64_t capacity,
                                  uint64_t ino,
                                  int64_t next_offset,
                                  uint8_t type,
                                  const char *name) {
    const size_t name_length = string_length(name) + 1u;
    size_t record_length = sizeof(struct plasma_linux_dirent64_head) + name_length;
    record_length = (record_length + 7u) & ~(size_t)7u;
    if (record_length > capacity || record_length > 512u) return 0;

    uint8_t record[512];
    bytes_zero(record, record_length);
    struct plasma_linux_dirent64_head head = {
        .ino = ino,
        .off = next_offset,
        .reclen = (uint16_t)record_length,
        .type = type,
    };
    bytes_copy(record, &head, sizeof(head));
    bytes_copy(record + sizeof(head), name, name_length);
    if (!user_copy_out(buffer_address, record, record_length)) return -LINUX_EFAULT;
    return (int64_t)record_length;
}

static void plasma_getdents_trace_value(const char *label, uint64_t value) {
    serial_write(label);
    serial_u64(value);
    serial_write("\n");
}

static int64_t plasma_getdents64(int fd, uint64_t buffer_address, uint64_t count) {
    struct rootfs_open_file *directory = rootfs_file_for_fd(fd);
    if (directory == 0) return -LINUX_EBADF;
    if ((directory->node.mode & PLASMA_MODE_MASK) != PLASMA_MODE_DIR) return -LINUX_ENOTDIR;
    if (count == 0 || !user_range(buffer_address, count, true)) return -LINUX_EFAULT;

    serial_write("[linux:getdents64] enter fd=");
    serial_u64((uint64_t)(unsigned int)fd);
    serial_write(" count=");
    serial_u64(count);
    serial_write(" path=");
    serial_write(directory->node.name != 0 ? directory->node.name : "<null>");
    serial_write("\n");

    uint64_t written = 0;
    uint64_t cursor = directory->offset;
    uint64_t iterations = 0;

    plasma_getdents_trace_value("[linux:getdents64] start cursor=", cursor);

    while (written < count) {
        if (++iterations > PLASMA_GETDENTS_RUNAWAY_LIMIT) {
            serial_write("[linux:getdents64] BUG: runaway directory enumeration\n");
            directory->offset = cursor;
            return -LINUX_EIO;
        }

        if (cursor == 0) {
            const int64_t rc = plasma_emit_dirent(buffer_address + written, count - written,
                                                   1, 1, PLASMA_DT_DIR, ".");
            if (rc <= 0) break;
            written += (uint64_t)rc;
            cursor = 1;
            continue;
        }
        if (cursor == 1) {
            const int64_t rc = plasma_emit_dirent(buffer_address + written, count - written,
                                                   1, 2, PLASMA_DT_DIR, "..");
            if (rc <= 0) break;
            written += (uint64_t)rc;
            cursor = 2;
            continue;
        }

        const size_t total = rootfs_entry_count();
        bool emitted = false;
        while ((size_t)(cursor - 2u) < total) {
            if (++iterations > PLASMA_GETDENTS_RUNAWAY_LIMIT) {
                serial_write("[linux:getdents64] BUG: runaway rootfs scan\n");
                directory->offset = cursor;
                return -LINUX_EIO;
            }

            const uint64_t before_cursor = cursor;
            const size_t index = (size_t)(cursor - 2u);

            if (index < 8u || (index % PLASMA_GETDENTS_TRACE_STRIDE) == 0u) {
                serial_write("[linux:getdents64] scan index=");
                serial_u64((uint64_t)index);
                serial_write(" cursor=");
                serial_u64(cursor);
                serial_write(" total=");
                serial_u64((uint64_t)total);
                serial_write("\n");
            }

            struct rootfs_node node;
            ++cursor;
            if (cursor <= before_cursor) {
                serial_write("[linux:getdents64] BUG: iterator did not advance\n");
                directory->offset = before_cursor;
                return -LINUX_EIO;
            }

            if (!rootfs_entry_at(index, &node)) {
                serial_write("[linux:getdents64] rootfs_entry_at failed index=");
                serial_u64((uint64_t)index);
                serial_write("\n");
                return -LINUX_EIO;
            }

            const char *child = 0;
            if (!plasma_immediate_child(directory->node.name, node.name, &child)) continue;

            serial_write("[linux:getdents64] child index=");
            serial_u64((uint64_t)index);
            serial_write(" name=");
            serial_write(child != 0 ? child : "<null>");
            serial_write("\n");

            const int64_t rc = plasma_emit_dirent(buffer_address + written, count - written,
                                                   index + 2u, (int64_t)cursor,
                                                   plasma_dirent_type(node.mode), child);
            if (rc < 0) return rc;
            if (rc == 0) {
                --cursor;
                directory->offset = cursor;
                serial_write("[linux:getdents64] output buffer full; saved cursor=");
                serial_u64(cursor);
                serial_write(" written=");
                serial_u64(written);
                serial_write("\n");
                return (int64_t)written;
            }
            written += (uint64_t)rc;
            emitted = true;
            break;
        }
        if (!emitted) break;
    }

    directory->offset = cursor;
    serial_write("[linux:getdents64] return bytes=");
    serial_u64(written);
    serial_write(" cursor=");
    serial_u64(cursor);
    serial_write(" iterations=");
    serial_u64(iterations);
    serial_write("\n");
    return (int64_t)written;
}

'''
    text = replace_once(
        text,
        "static int64_t shell_dispatch(uint64_t number,\n",
        code + "static int64_t shell_dispatch(uint64_t number,\n",
    )
    text = replace_once(
        text,
        "    case SYS_GETDENTS64: return 0;\n",
        "    case SYS_GETDENTS64: return plasma_getdents64((int)a1, a2, a3);\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Added CPIO getdents64 directory enumeration + bounded diagnostics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
