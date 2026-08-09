#!/usr/bin/env python3
"""Build the Alpine 3.21 x86_64 Plasma/X11 rootfs natively with apko.

This path is intended primarily for macOS.  It uses Homebrew's native apko
binary to resolve Alpine APK dependencies and asks apko's build-minirootfs
command for an x86_64 filesystem directly.  No Docker daemon, Linux VM, or
container image store is required.

The temporary apko tarball is deleted immediately after extraction, and the
extracted tree is removed after the final Twilight CPIO is written to keep peak
and persistent disk usage as small as practical.
"""

from __future__ import annotations

import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tarfile

VERSION = "3.21.7"
BRANCH = "v3.21"
MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
MAGIC = b"070701"

# Keep icu-data-full as an explicit top-level constraint.  Alpine's icu-libs
# dependency is on the virtual provider "icu-data" and normally resolves to the
# much smaller icu-data-en package.  Qt6 Qt5Compat, pulled in by Plasma, requires
# icu-data-full specifically.  Giving the full provider to apko up front avoids
# committing the solver to icu-data-en before it reaches that Qt dependency.
PLASMA_PACKAGES = [
    "icu-data-full",
    "alpine-base",
    "plasma-desktop",
    "plasma-workspace-x11",
    "xorg-server",
    "xf86-video-fbdev",
    "xinit",
    "dbus",
]


def field(value: int) -> bytes:
    return f"{value & 0xffffffff:08x}".encode("ascii")


def write_pad4(out, length: int) -> None:
    padding = (-length) & 3
    if padding:
        out.write(b"\0" * padding)


def write_entry_header(out, ino: int, name: str, mode: int, size: int) -> None:
    name_b = name.encode("utf-8", "surrogateescape") + b"\0"
    header = b"".join(
        [
            MAGIC,
            field(ino), field(mode), field(0), field(0),
            field(2 if stat.S_ISDIR(mode) else 1), field(0), field(size),
            field(0), field(0), field(0), field(0), field(len(name_b)), field(0),
        ]
    )
    if len(header) != 110:
        raise RuntimeError("internal CPIO header size error")
    out.write(header)
    out.write(name_b)
    write_pad4(out, 110 + len(name_b))


def extract_trusted_tar(archive: pathlib.Path, destination: pathlib.Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(archive, "r:*") as tf:
        # apko just produced this archive locally from the configured Alpine
        # repositories.  Alpine deliberately contains absolute BusyBox links
        # such as /usr/bin/yes -> /bin/busybox, so Python 3.14's default data
        # filter is too restrictive for this known rootfs archive.
        try:
            tf.extractall(destination, filter="fully_trusted")
        except TypeError:
            tf.extractall(destination)


def write_apko_config(path: pathlib.Path) -> None:
    package_lines = "".join(f"    - {package}\n" for package in PLASMA_PACKAGES)
    path.write_text(
        "contents:\n"
        "  repositories:\n"
        f"    - {MIRROR}/{BRANCH}/main\n"
        f"    - {MIRROR}/{BRANCH}/community\n"
        "  packages:\n"
        f"{package_lines}"
        "environment:\n"
        "  PATH: /usr/local/sbin:/usr/local/bin:/usr/bin:/usr/sbin:/sbin:/bin\n",
        encoding="ascii",
    )


def build_with_apko(work: pathlib.Path, target_root: pathlib.Path) -> None:
    apko = shutil.which("apko")
    if apko is None:
        raise RuntimeError(
            "macOS Plasma rootfs build needs apko, not Docker. Install it with: brew install apko"
        )

    work.mkdir(parents=True, exist_ok=True)
    config = work / "plasma-x11.apko.yaml"
    tarball = work / "plasma-x11-x86_64.tar.gz"
    write_apko_config(config)

    print("Building Alpine Plasma/X11 x86_64 minirootfs natively with apko")
    print("Packages:", " ".join(PLASMA_PACKAGES))
    command = [
        apko,
        "build-minirootfs",
        "--build-arch", "x86_64",
        str(config),
        str(tarball),
    ]
    result = subprocess.run(command, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"apko build-minirootfs failed with exit code {result.returncode}")
    if not tarball.exists() or tarball.stat().st_size == 0:
        raise RuntimeError("apko reported success but did not produce the minirootfs tarball")

    print(f"Extracting apko minirootfs ({tarball.stat().st_size // (1024 * 1024)} MiB compressed)")
    extract_trusted_tar(tarball, target_root)
    tarball.unlink(missing_ok=True)
    print("Removed temporary apko tarball to save disk space")


def write_nox_configuration(root: pathlib.Path) -> None:
    (root / "etc/X11/xorg.conf.d").mkdir(parents=True, exist_ok=True)
    (root / "etc/X11/xorg.conf.d/20-twilight-fbdev.conf").write_text(
        '''Section "Device"\n'''
        '''    Identifier "TwilightFramebuffer"\n'''
        '''    Driver "fbdev"\n'''
        '''    Option "fbdev" "/dev/fb0"\n'''
        '''EndSection\n\n'''
        '''Section "ServerFlags"\n'''
        '''    Option "AutoAddDevices" "false"\n'''
        '''EndSection\n''',
        encoding="ascii",
    )

    (root / "root").mkdir(parents=True, exist_ok=True)
    (root / "root/.xinitrc").write_text(
        "export XDG_RUNTIME_DIR=/tmp/runtime-root\n"
        "export KWIN_COMPOSE=N\n"
        "exec dbus-run-session startplasma-x11\n",
        encoding="ascii",
    )

    (root / "etc/nox-release").write_text(
        "NAME=Nox\n"
        "KERNEL=Twilight\n"
        f"ALPINE={VERSION}\n"
        "PLASMA=6.2\n"
        "SESSION=X11-fbdev\n"
        "USERSPACE_STAGE=plasma-gui-first-launch\n",
        encoding="ascii",
    )


def verify_rootfs(root: pathlib.Path) -> None:
    required = [
        "bin/busybox",
        "lib/ld-musl-x86_64.so.1",
        "usr/bin/Xorg",
        "usr/bin/startplasma-x11",
        "usr/bin/dbus-run-session",
    ]
    # Use lexists rather than Path.exists(): Alpine legitimately uses absolute
    # symlinks inside the rootfs, and Path.exists() would follow those against
    # the macOS host root instead of checking the extracted filesystem entry.
    missing = [entry for entry in required if not os.path.lexists(root / entry)]
    if missing:
        raise RuntimeError("apko rootfs is missing required GUI files: " + ", ".join(missing))
    print("apko rootfs sanity check PASS: BusyBox + musl + Xorg + Plasma X11 + D-Bus")


def normalized_rel(path: pathlib.Path, root: pathlib.Path) -> str:
    rel = path.relative_to(root).as_posix()
    return rel if rel != "." else "."


def stream_tree_as_cpio(root: pathlib.Path, output: pathlib.Path) -> tuple[int, int]:
    entries: list[pathlib.Path] = [root]
    for directory, dirnames, filenames in os.walk(root, topdown=True, followlinks=False):
        directory_path = pathlib.Path(directory)
        dirnames.sort()
        filenames.sort()
        for name in dirnames:
            entries.append(directory_path / name)
        for name in filenames:
            entries.append(directory_path / name)

    ino = 1
    count = 0
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as out:
        for path in entries:
            name = normalized_rel(path, root)
            if name == "TRAILER!!!":
                continue
            st = path.lstat()
            mode = st.st_mode
            data: bytes | None = None
            size = 0
            if stat.S_ISLNK(mode):
                data = os.readlink(path).encode("utf-8", "surrogateescape")
                size = len(data)
            elif stat.S_ISREG(mode):
                size = st.st_size
            elif stat.S_ISSOCK(mode):
                continue

            write_entry_header(out, ino, name, mode, size)
            if data is not None:
                out.write(data)
            elif stat.S_ISREG(mode) and size:
                with path.open("rb") as source:
                    while True:
                        chunk = source.read(1024 * 1024)
                        if not chunk:
                            break
                        out.write(chunk)
            write_pad4(out, size)
            ino += 1
            count += 1

        write_entry_header(out, ino, "TRAILER!!!", 0, 0)
        write_pad4(out, 0)

    return count, output.stat().st_size


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT.cpio", file=sys.stderr)
        return 2

    output = pathlib.Path(sys.argv[1])
    work = output.parent / "plasma-apko-build"
    target_root = work / "root-x86_64"

    if work.exists():
        shutil.rmtree(work)

    try:
        build_with_apko(work, target_root)
        write_nox_configuration(target_root)
        verify_rootfs(target_root)
        count, size = stream_tree_as_cpio(target_root, output)
        print(f"Created native-apko Alpine Plasma X11 CPIO rootfs: {output}")
        print(f"Archive size: {size // (1024 * 1024)} MiB")
        print(f"Entries: {count}")
        print("First GUI target: Xorg fbdev, then dbus-run-session startplasma-x11")
    finally:
        if work.exists():
            shutil.rmtree(work)
            print("Removed temporary extracted Plasma tree to save disk space")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
