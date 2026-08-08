#!/usr/bin/env python3
"""Fetch Debian's x86_64 GNU Bash 5.3 static Linux executable.

The Bash executable itself is not rewritten.  We extract /usr/bin/bash-static
from Debian's bash-static package and verify that it is an x86_64 ELF64 static
executable (no PT_INTERP) before handing the exact bytes to Twilight's embedder.
"""

from __future__ import annotations

import hashlib
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request

URL = "https://ftp.debian.org/debian/pool/main/b/bash/bash-static_5.3-3_amd64.deb"
PACKAGE_VERSION = "5.3-3"
PT_INTERP = 3
ET_EXEC = 2
EM_X86_64 = 62


def verify_elf(data: bytes) -> None:
    if len(data) < 64 or data[:4] != b"\x7fELF":
        raise RuntimeError("extracted bash-static is not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise RuntimeError("bash-static is not little-endian ELF64")

    e_type, e_machine = struct.unpack_from("<HH", data, 16)
    if e_type != ET_EXEC:
        raise RuntimeError(f"bash-static ELF type is {e_type}, expected ET_EXEC")
    if e_machine != EM_X86_64:
        raise RuntimeError(f"bash-static e_machine is {e_machine}, expected x86_64")

    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]
    if e_phentsize < 56 or e_phoff + e_phentsize * e_phnum > len(data):
        raise RuntimeError("invalid Bash ELF program-header table")

    for index in range(e_phnum):
        p_type = struct.unpack_from("<I", data, e_phoff + index * e_phentsize)[0]
        if p_type == PT_INTERP:
            raise RuntimeError("bash-static unexpectedly contains PT_INTERP")


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} OUTPUT", file=sys.stderr)
        return 2

    if shutil.which("dpkg-deb") is None:
        print("ERROR: dpkg-deb is required to extract Debian bash-static", file=sys.stderr)
        return 1

    output = pathlib.Path(sys.argv[1])
    output.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="twilight-bash-") as temp_name:
        temp = pathlib.Path(temp_name)
        package = temp / "bash-static.deb"
        root = temp / "root"

        request = urllib.request.Request(
            URL,
            headers={"User-Agent": "Twilight-Nox-Bash-compat/1.0"},
        )
        print(f"Fetching Debian GNU Bash static {PACKAGE_VERSION} amd64 from {URL}")
        with urllib.request.urlopen(request, timeout=90) as response:
            package.write_bytes(response.read())

        subprocess.run(
            ["dpkg-deb", "-x", str(package), str(root)],
            check=True,
            stdout=subprocess.DEVNULL,
        )
        source = root / "usr/bin/bash-static"
        if not source.is_file():
            raise RuntimeError("Debian package did not contain /usr/bin/bash-static")

        data = source.read_bytes()
        verify_elf(data)
        output.write_bytes(data)

    digest = hashlib.sha256(data).hexdigest()
    print(f"Extracted GNU Bash static unchanged: {output} ({len(data)} bytes)")
    print(f"Bash SHA-256: {digest}")
    print("Verified: ELF64 x86_64 ET_EXEC with no PT_INTERP")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
