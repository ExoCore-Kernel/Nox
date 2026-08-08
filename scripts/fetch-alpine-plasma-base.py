#!/usr/bin/env python3
"""Fetch Alpine x86_64 minirootfs and repack it as Twilight CPIO newc.

This is the next userspace stage after the tiny synthetic Plasma rootfs. Alpine
3.24 uses musl and is therefore a convenient bridge from Twilight's existing
BusyBox/musl Linux ABI toward Qt/KDE. Plasma packages are intentionally added in
a later step once dynamic ELF + process/file syscalls can execute Alpine tools.
"""

from __future__ import annotations

import hashlib
import pathlib
import stat
import sys
import tarfile
import urllib.request

VERSION = "3.24.1"
BASE = "https://dl-cdn.alpinelinux.org/alpine/v3.24/releases/x86_64"
FILENAME = f"alpine-minirootfs-{VERSION}-x86_64.tar.gz"
URL = f"{BASE}/{FILENAME}"
SHA_URL = f"{URL}.sha256"
MAGIC = b"070701"


def pad4(buf: bytearray) -> None:
    while len(buf) & 3:
        buf.append(0)


def field(value: int) -> bytes:
    return f"{value & 0xffffffff:08x}".encode("ascii")


def add_entry(buf: bytearray, ino: int, name: str, mode: int, data: bytes = b"") -> None:
    name_b = name.encode("utf-8", "surrogateescape") + b"\0"
    header = b"".join(
        [
            MAGIC,
            field(ino), field(mode), field(0), field(0),
            field(2 if stat.S_ISDIR(mode) else 1),
            field(0), field(len(data)),
            field(0), field(0), field(0), field(0),
            field(len(name_b)), field(0),
        ]
    )
    if len(header) != 110:
        raise RuntimeError("internal CPIO header size error")
    buf.extend(header)
    buf.extend(name_b)
    pad4(buf)
    buf.extend(data)
    pad4(buf)


def normalized_name(name: str) -> str:
    while name.startswith("./"):
        name = name[2:]
    name = name.lstrip("/")
    return name or "."


def tar_mode(member: tarfile.TarInfo) -> int:
    perms = member.mode & 0o7777
    if member.isdir():
        return stat.S_IFDIR | perms
    if member.issym():
        return stat.S_IFLNK | perms
    if member.ischr():
        return stat.S_IFCHR | perms
    if member.isblk():
        return stat.S_IFBLK | perms
    if member.isfifo():
        return stat.S_IFIFO | perms
    return stat.S_IFREG | perms


def download_checked(cache: pathlib.Path) -> pathlib.Path:
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / FILENAME
    print(f"Fetching Alpine {VERSION} x86_64 minirootfs")
    with urllib.request.urlopen(SHA_URL, timeout=60) as response:
        expected = response.read().decode("ascii").split()[0].lower()

    if not archive.exists():
        with urllib.request.urlopen(URL, timeout=120) as response:
            archive.write_bytes(response.read())

    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != expected:
        archive.unlink(missing_ok=True)
        raise RuntimeError(f"Alpine SHA-256 mismatch: expected {expected}, got {actual}")
    print(f"Verified SHA-256: {actual}")
    return archive


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT.cpio", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)
    cache = output.parent / "downloads"
    source = download_checked(cache)

    cpio = bytearray()
    ino = 1
    seen: set[str] = set()

    with tarfile.open(source, "r:gz") as tf:
        members = sorted(tf.getmembers(), key=lambda m: normalized_name(m.name))
        for member in members:
            name = normalized_name(member.name)
            if name in seen or name == "TRAILER!!!":
                continue
            seen.add(name)
            mode = tar_mode(member)
            data = b""
            if member.issym():
                data = member.linkname.encode("utf-8", "surrogateescape")
            elif member.isfile() or member.islnk():
                stream = tf.extractfile(member)
                if stream is not None:
                    data = stream.read()
            add_entry(cpio, ino, name, mode, data)
            ino += 1

    # Nox-specific marker used by the bring-up tests and eventually the session
    # launcher. Do not overwrite an Alpine-provided file if one ever appears.
    if "etc/nox-release" not in seen:
        add_entry(
            cpio,
            ino,
            "etc/nox-release",
            stat.S_IFREG | 0o644,
            (
                f"NAME=Nox\nKERNEL=Twilight\nALPINE={VERSION}\n"
                "USERSPACE_STAGE=alpine-dynamic-elf\n"
            ).encode("ascii"),
        )
        ino += 1

    add_entry(cpio, ino, "TRAILER!!!", 0, b"")
    output.write_bytes(cpio)

    print(f"Created Alpine CPIO rootfs: {output}")
    print(f"Archive size: {len(cpio) // 1024} KiB")
    print(f"Entries: {ino - 1}")
    print("Next gate: launch Alpine /lib/ld-musl-x86_64.so.1 and /bin/busybox from rootfs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
