#!/usr/bin/env python3
"""Create Twilight's first Plasma-oriented initramfs.

This starts deliberately tiny. The kernel/ABI work should first prove that a
real bootloader-supplied filesystem can be mounted and read. Later this archive
can be replaced with a packaged musl/Qt/KDE userspace without changing the
kernel-facing rootfs interface.
"""

from __future__ import annotations

import pathlib
import stat
import sys

MAGIC = b"070701"


def pad4(data: bytearray) -> None:
    while len(data) & 3:
        data.append(0)


def field(value: int) -> bytes:
    return f"{value & 0xffffffff:08x}".encode("ascii")


def add_entry(archive: bytearray, ino: int, name: str, mode: int, data: bytes = b"") -> None:
    encoded_name = name.encode("utf-8") + b"\0"
    header = b"".join(
        [
            MAGIC,
            field(ino),
            field(mode),
            field(0),  # uid
            field(0),  # gid
            field(2 if stat.S_ISDIR(mode) else 1),
            field(0),  # mtime: deterministic
            field(len(data)),
            field(0), field(0), field(0), field(0),
            field(len(encoded_name)),
            field(0),  # checksum for newc
        ]
    )
    assert len(header) == 110
    archive.extend(header)
    archive.extend(encoded_name)
    pad4(archive)
    archive.extend(data)
    pad4(archive)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)

    archive = bytearray()
    entries = [
        (".", stat.S_IFDIR | 0o755, b""),
        ("bin", stat.S_IFDIR | 0o755, b""),
        ("dev", stat.S_IFDIR | 0o755, b""),
        ("etc", stat.S_IFDIR | 0o755, b""),
        ("lib", stat.S_IFDIR | 0o755, b""),
        ("lib64", stat.S_IFDIR | 0o755, b""),
        ("proc", stat.S_IFDIR | 0o555, b""),
        ("run", stat.S_IFDIR | 0o755, b""),
        ("sys", stat.S_IFDIR | 0o555, b""),
        ("tmp", stat.S_IFDIR | 0o1777, b""),
        ("usr", stat.S_IFDIR | 0o755, b""),
        ("usr/bin", stat.S_IFDIR | 0o755, b""),
        ("usr/lib", stat.S_IFDIR | 0o755, b""),
        (
            "etc/nox-release",
            stat.S_IFREG | 0o644,
            b"NAME=Nox\nKERNEL=Twilight\nUSERSPACE_STAGE=plasma-rootfs-1\n",
        ),
        (
            "etc/plasma-bringup",
            stat.S_IFREG | 0o644,
            b"Goal: real rootfs -> dynamic ELF -> Qt -> Wayland/KWin -> plasmashell\n",
        ),
    ]

    ino = 1
    for name, mode, data in entries:
        add_entry(archive, ino, name, mode, data)
        ino += 1
    add_entry(archive, ino, "TRAILER!!!", 0, b"")

    output.write_bytes(archive)
    print(f"Created Plasma bring-up initramfs: {output} ({len(archive)} bytes)")
    print(f"Entries: {len(entries)} + TRAILER!!!")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
