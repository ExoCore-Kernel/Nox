#!/usr/bin/env python3
"""Build an Alpine 3.21 x86_64 Plasma/X11 CPIO rootfs cross-platform.

On Linux/aarch64, use the matching Alpine aarch64 minirootfs and invoke its
native apk through musl while asking apk to populate an x86_64 target root.
On macOS (or other non-Linux hosts), use Docker/Podman only for that apk install
step. Downloading, configuration, and CPIO generation remain native Python.
"""

from __future__ import annotations

import hashlib
import os
import pathlib
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import urllib.request

VERSION = "3.21.7"
BRANCH = "v3.21"
MIRROR = "https://dl-cdn.alpinelinux.org/alpine"
RELEASE_BASE = f"{MIRROR}/{BRANCH}/releases"
MAGIC = b"070701"

PLASMA_PACKAGES = [
    "plasma-desktop",
    "plasma-workspace-x11",
    "xorg-server",
    "xf86-video-fbdev",
    "xf86-input-mouse",
    "xf86-input-keyboard",
    "xinit",
    "dbus",
]


def field(value: int) -> bytes:
    return f"{value & 0xffffffff:08x}".encode("ascii")


def write_pad4(out, length: int) -> None:
    padding = (-length) & 3
    if padding:
        out.write(b"\0" * padding)


def write_entry_header(out, ino: int, name: str, mode: int, size: int) -> int:
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
    return len(name_b)


def download_release(cache: pathlib.Path, arch: str) -> pathlib.Path:
    cache.mkdir(parents=True, exist_ok=True)
    filename = f"alpine-minirootfs-{VERSION}-{arch}.tar.gz"
    url = f"{RELEASE_BASE}/{arch}/{filename}"
    sha_url = f"{url}.sha256"
    archive = cache / filename

    print(f"Fetching Alpine {VERSION} {arch} minirootfs")
    with urllib.request.urlopen(sha_url, timeout=60) as response:
        expected = response.read().decode("ascii").split()[0].lower()
    if not archive.exists():
        with urllib.request.urlopen(url, timeout=180) as response:
            archive.write_bytes(response.read())
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if actual != expected:
        archive.unlink(missing_ok=True)
        raise RuntimeError(f"SHA-256 mismatch for {filename}: expected {expected}, got {actual}")
    print(f"Verified {filename}: {actual}")
    return archive


def extract_rootfs(archive: pathlib.Path, destination: pathlib.Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    with tarfile.open(archive, "r:gz") as tf:
        try:
            tf.extractall(destination, filter="fully_trusted")
        except TypeError:
            tf.extractall(destination)


def write_repositories(target_root: pathlib.Path) -> pathlib.Path:
    repositories = target_root / "etc/apk/repositories"
    repositories.parent.mkdir(parents=True, exist_ok=True)
    repositories.write_text(
        f"{MIRROR}/{BRANCH}/main\n{MIRROR}/{BRANCH}/community\n",
        encoding="ascii",
    )
    return repositories


def apk_arguments(root: str, repositories: str) -> list[str]:
    return [
        "--root", root,
        "--arch", "x86_64",
        "--repositories-file", repositories,
        "--no-cache",
        "--no-scripts",
        "--no-chown",
        "add",
        *PLASMA_PACKAGES,
    ]


def run_native_apk(tool_root: pathlib.Path, target_root: pathlib.Path) -> None:
    loader = tool_root / "lib/ld-musl-aarch64.so.1"
    apk = tool_root / "sbin/apk"
    if not loader.exists() or not apk.exists():
        raise RuntimeError("aarch64 Alpine tool root does not contain musl loader + /sbin/apk")

    repositories = write_repositories(target_root)
    library_path = f"{tool_root}/lib:{tool_root}/usr/lib"
    command = [
        str(loader), "--library-path", library_path, str(apk),
        *apk_arguments(str(target_root), str(repositories)),
    ]
    print("Installing Alpine Plasma/X11 dependency closure with native aarch64 apk")
    print("Packages:", " ".join(PLASMA_PACKAGES))
    result = subprocess.run(command, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"native aarch64 apk failed with exit code {result.returncode}")


def find_container_runtime() -> str | None:
    for name in ("docker", "podman"):
        executable = shutil.which(name)
        if executable:
            return executable
    return None


def run_container_apk(target_root: pathlib.Path) -> None:
    runtime = find_container_runtime()
    if runtime is None:
        raise RuntimeError(
            "macOS Plasma rootfs build needs Docker Desktop or Podman for the Alpine apk step. "
            "Install/start one, then rerun the same command."
        )

    write_repositories(target_root)
    target = target_root.resolve()
    command = [
        runtime,
        "run", "--rm",
        "-v", f"{target}:/target",
        "alpine:3.21",
        "/sbin/apk",
        *apk_arguments("/target", "/target/etc/apk/repositories"),
    ]
    print(f"Installing Alpine Plasma/X11 dependency closure via {pathlib.Path(runtime).name}")
    print("Packages:", " ".join(PLASMA_PACKAGES))
    result = subprocess.run(command, text=True)
    if result.returncode != 0:
        raise RuntimeError(
            f"containerized apk failed with exit code {result.returncode}; "
            "make sure the container runtime is running"
        )


def install_plasma_packages(cache: pathlib.Path, work: pathlib.Path,
                            target_root: pathlib.Path) -> None:
    system = platform.system().lower()
    machine = platform.machine().lower()

    if system == "linux" and machine in ("aarch64", "arm64"):
        arm_archive = download_release(cache, "aarch64")
        tool_root = work / "tool-aarch64"
        extract_rootfs(arm_archive, tool_root)
        run_native_apk(tool_root, target_root)
        return

    run_container_apk(target_root)


def write_nox_configuration(root: pathlib.Path) -> None:
    (root / "etc/X11/xorg.conf.d").mkdir(parents=True, exist_ok=True)
    (root / "etc/X11/xorg.conf.d/20-twilight-fbdev.conf").write_text(
        '''Section "Module"\n'''
        '''    Disable "glx"\n'''
        '''    Load "fbdevhw"\n'''
        '''    Load "shadow"\n'''
        '''EndSection\n\n'''
        '''Section "Device"\n'''
        '''    Identifier "TwilightFramebuffer"\n'''
        '''    Driver "fbdev"\n'''
        '''    Option "fbdev" "/dev/fb0"\n'''
        '''EndSection\n\n'''
        '''Section "Monitor"\n'''
        '''    Identifier "TwilightMonitor"\n'''
        '''EndSection\n\n'''
        '''Section "Screen"\n'''
        '''    Identifier "TwilightScreen"\n'''
        '''    Device "TwilightFramebuffer"\n'''
        '''    Monitor "TwilightMonitor"\n'''
        '''    DefaultDepth 32\n'''
        '''EndSection\n\n'''
        '''Section "ServerLayout"\n'''
        '''    Identifier "TwilightLayout"\n'''
        '''    Screen 0 "TwilightScreen" 0 0\n'''
        '''EndSection\n\n'''
        '''Section "ServerFlags"\n'''
        '''    Option "AutoAddDevices" "false"\n'''
        '''    Option "AutoAddGPU" "false"\n'''
        '''    Option "AutoBindGPU" "false"\n'''
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


def normalized_rel(path: pathlib.Path, root: pathlib.Path) -> str:
    rel = path.relative_to(root).as_posix()
    return rel if rel != "." else "."


def cpio_mode(st: os.stat_result) -> int:
    return st.st_mode


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
            mode = cpio_mode(st)
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
    work = output.parent / "plasma-x11-build"
    cache = output.parent / "downloads"
    target_root = work / "root-x86_64"

    if platform.system().lower() != "linux" and find_container_runtime() is None:
        raise RuntimeError(
            "This host cannot run Alpine apk natively. Install/start Docker Desktop or Podman, "
            "then rerun scripts/run-plasma-bringup.sh gui."
        )

    x86_archive = download_release(cache, "x86_64")
    extract_rootfs(x86_archive, target_root)
    install_plasma_packages(cache, work, target_root)
    write_nox_configuration(target_root)

    count, size = stream_tree_as_cpio(target_root, output)
    print(f"Created Alpine Plasma X11 CPIO rootfs: {output}")
    print(f"Archive size: {size // (1024 * 1024)} MiB")
    print(f"Entries: {count}")
    print("First GUI target: Xorg fbdev, then dbus-run-session startplasma-x11")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
