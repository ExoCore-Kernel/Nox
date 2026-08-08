#!/usr/bin/env python3
"""Probe upstream Linux drivers against Twilight's current compatibility layer.

This is deliberately a discovery tool: upstream source is downloaded unchanged,
each translation unit is compiled independently against twilight/include, and
all failures are collected instead of stopping at the first driver.  Quoted
headers that live beside a driver are downloaded too, but Linux's own global
include tree is *not* added to the compiler search path; <linux/...> and
<asm/...> must therefore be provided by Twilight's compatibility boundary.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

REPO = "torvalds/linux"
API = f"https://api.github.com/repos/{REPO}/contents"
RAW = f"https://raw.githubusercontent.com/{REPO}"

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BUILD = ROOT / "build" / "upstream-driver-probe"

COMMON_CFLAGS = [
    "-target", "x86_64-unknown-none-elf",
    "-std=gnu11",
    "-O0",
    "-Wall", "-Wextra",
    "-ffreestanding",
    "-fno-builtin",
    "-fno-stack-protector",
    "-fno-stack-check",
    "-fno-lto",
    "-fno-pic",
    "-fno-pie",
    "-m64",
    "-march=x86-64",
    "-mno-red-zone",
    "-mno-mmx",
    "-mno-sse",
    "-mno-sse2",
    "-mcmodel=kernel",
    f"-I{ROOT / 'twilight' / 'include'}",
]

# Keep this aligned with scripts/run-hardware-matrix.sh.  Versions are chosen
# to keep subsystem complexity as low as reasonably possible while still
# containing a driver for the QEMU device we test.
DRIVERS = [
    {
        "name": "rtl8139",
        "tag": "v2.6.24",
        "files": ["drivers/net/8139too.c"],
        "compile": ["drivers/net/8139too.c"],
        "kbuild": "8139too",
        "qemu": "rtl8139",
        "pci": "10ec:8139",
        "integrated": True,
    },
    {
        "name": "ahci",
        "tag": "v2.6.24",
        "files": ["drivers/ata/ahci.c"],
        "compile": ["drivers/ata/ahci.c"],
        "kbuild": "ahci",
        "qemu": "ICH9 AHCI (q35 built-in)",
        "pci": "8086:2922",
        "integrated": True,
    },
    {
        "name": "e1000",
        "tag": "v2.6.24",
        "dirs": ["drivers/net/e1000"],
        "compile_globs": ["drivers/net/e1000/*.c"],
        "kbuild": "e1000",
        "qemu": "e1000",
        "pci": "8086:100e",
    },
    {
        "name": "e1000e",
        "tag": "v2.6.24",
        "dirs": ["drivers/net/e1000e"],
        "compile_globs": ["drivers/net/e1000e/*.c"],
        "kbuild": "e1000e",
        "qemu": "e1000e",
        "pci": "8086:10d3",
    },
    {
        "name": "virtio-net",
        "tag": "v2.6.24",
        "files": ["drivers/net/virtio_net.c"],
        "compile": ["drivers/net/virtio_net.c"],
        "kbuild": "virtio_net",
        "qemu": "virtio-net-pci",
        "pci": "1af4:1000",
    },
    {
        "name": "virtio-blk",
        "tag": "v2.6.24",
        "files": ["drivers/block/virtio_blk.c"],
        "compile": ["drivers/block/virtio_blk.c"],
        "kbuild": "virtio_blk",
        "qemu": "virtio-blk-pci",
        "pci": "1af4:1001",
    },
    {
        "name": "nvme",
        "tag": "v3.3",
        "files": ["drivers/block/nvme.c"],
        "compile": ["drivers/block/nvme.c"],
        "kbuild": "nvme",
        "qemu": "nvme",
        "pci": "1b36:0010",
    },
    {
        "name": "xhci",
        "tag": "v2.6.31",
        "dirs": ["drivers/usb/host"],
        "compile_globs": ["drivers/usb/host/xhci*.c"],
        "kbuild": "xhci_hcd",
        "qemu": "qemu-xhci",
        "pci": "1b36:000d",
    },
    {
        "name": "hda-intel",
        "tag": "v2.6.24",
        "dirs": ["sound/pci/hda"],
        "compile": ["sound/pci/hda/hda_intel.c"],
        "kbuild": "snd_hda_intel",
        "qemu": "ich9-intel-hda",
        "pci": "8086:293e",
    },
    {
        "name": "virtio-gpu",
        "tag": "v4.2",
        "dirs": ["drivers/gpu/drm/virtio"],
        "compile_globs": ["drivers/gpu/drm/virtio/*.c"],
        "kbuild": "virtio_gpu",
        "qemu": "virtio-gpu-pci",
        "pci": "1af4:1050",
    },
]


def request_bytes(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": "Nox-Upstream-Driver-Probe/1"})
    with urllib.request.urlopen(req, timeout=45) as response:
        return response.read()


def request_json(url: str):
    return json.loads(request_bytes(url).decode("utf-8"))


def git_blob_sha1(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode("ascii") + data).hexdigest()


def fetch_file(tag: str, path: str, source_root: Path, manifest: dict[str, dict]) -> None:
    dest = source_root / path
    if dest.exists():
        return
    url = f"{RAW}/{urllib.parse.quote(tag, safe='')}/{path}"
    data = request_bytes(url)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    manifest[f"{tag}:{path}"] = {
        "tag": tag,
        "path": path,
        "git_blob_sha1": git_blob_sha1(data),
        "bytes": len(data),
        "url": url,
    }


def fetch_directory(tag: str, path: str, source_root: Path, manifest: dict[str, dict]) -> None:
    query = urllib.parse.urlencode({"ref": tag})
    entries = request_json(f"{API}/{path}?{query}")
    if not isinstance(entries, list):
        raise RuntimeError(f"GitHub contents API did not return a directory for {tag}:{path}")
    for entry in entries:
        kind = entry.get("type")
        child_path = entry.get("path")
        if not child_path:
            continue
        if kind == "file":
            fetch_file(tag, child_path, source_root, manifest)
        elif kind == "dir":
            fetch_directory(tag, child_path, source_root, manifest)


def driver_sources(driver: dict, source_root: Path) -> list[Path]:
    result: list[Path] = []
    for rel in driver.get("compile", []):
        result.append(source_root / rel)
    for pattern in driver.get("compile_globs", []):
        result.extend(sorted(source_root.glob(pattern)))
    # Preserve order while avoiding duplicates.
    seen: set[Path] = set()
    unique: list[Path] = []
    for path in result:
        if path not in seen:
            seen.add(path)
            unique.append(path)
    return unique


def first_useful_lines(text: str, limit: int = 24) -> list[str]:
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    if len(lines) <= limit:
        return lines
    return lines[:limit] + [f"... {len(lines) - limit} more compiler line(s) in log"]


def compile_driver(clang: str, driver: dict, source_root: Path, obj_root: Path, log_root: Path) -> dict:
    name = driver["name"]
    sources = driver_sources(driver, source_root)
    if not sources:
        return {
            "name": name,
            "status": "NO_SOURCE",
            "failures": ["no translation units matched the candidate manifest"],
        }

    failures: list[str] = []
    compiled = 0
    driver_obj_root = obj_root / name
    driver_log_root = log_root / name
    driver_obj_root.mkdir(parents=True, exist_ok=True)
    driver_log_root.mkdir(parents=True, exist_ok=True)

    for source in sources:
        rel = source.relative_to(source_root)
        object_path = driver_obj_root / (str(rel).replace("/", "__") + ".o")
        log_path = driver_log_root / (str(rel).replace("/", "__") + ".log")
        cmd = [
            clang,
            *COMMON_CFLAGS,
            f"-DKBUILD_MODNAME=\"{driver['kbuild']}\"",
            "-c", str(source),
            "-o", str(object_path),
        ]
        proc = subprocess.run(cmd, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log_path.write_text(proc.stdout, encoding="utf-8")
        if proc.returncode == 0:
            compiled += 1
        else:
            preview = "\n".join(first_useful_lines(proc.stdout))
            failures.append(f"{rel}:\n{preview}")

    return {
        "name": name,
        "status": "COMPILE_PASS" if not failures else "COMPILE_FAIL",
        "compiled": compiled,
        "total": len(sources),
        "failures": failures,
        "integrated": bool(driver.get("integrated")),
        "qemu": driver["qemu"],
        "pci": driver["pci"],
        "tag": driver["tag"],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--strict", action="store_true", help="return non-zero when any candidate fails")
    parser.add_argument("--only", action="append", default=[], help="probe only one named candidate; may be repeated")
    args = parser.parse_args()

    clang = shutil.which(os.environ.get("CC", "clang"))
    if not clang:
        print("error: clang not found", file=sys.stderr)
        return 2

    selected = DRIVERS
    if args.only:
        wanted = set(args.only)
        selected = [driver for driver in DRIVERS if driver["name"] in wanted]
        missing = wanted - {driver["name"] for driver in selected}
        if missing:
            print(f"error: unknown driver(s): {', '.join(sorted(missing))}", file=sys.stderr)
            return 2

    build_dir = args.build_dir.resolve()
    source_root = build_dir / "source"
    obj_root = build_dir / "obj"
    log_root = build_dir / "logs"
    source_root.mkdir(parents=True, exist_ok=True)
    obj_root.mkdir(parents=True, exist_ok=True)
    log_root.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, dict] = {}
    results: list[dict] = []

    print(f"Upstream probe build dir: {build_dir}")
    print(f"Twilight compatibility headers: {ROOT / 'twilight' / 'include'}")
    print("")

    for driver in selected:
        name = driver["name"]
        print(f"===== {name} ({driver['tag']}, QEMU {driver['qemu']}, PCI {driver['pci']}) =====")
        try:
            for path in driver.get("files", []):
                fetch_file(driver["tag"], path, source_root, manifest)
            for path in driver.get("dirs", []):
                fetch_directory(driver["tag"], path, source_root, manifest)
        except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError, RuntimeError) as exc:
            print(f"FETCH_FAIL: {exc}")
            results.append({"name": name, "status": "FETCH_FAIL", "error": str(exc)})
            print("")
            continue

        result = compile_driver(clang, driver, source_root, obj_root, log_root)
        results.append(result)
        print(f"{result['status']}: {result.get('compiled', 0)}/{result.get('total', 0)} translation unit(s) compiled")
        if result.get("failures"):
            # Keep the console useful: print only the first failing TU. Full logs
            # remain in build/upstream-driver-probe/logs/.
            print(result["failures"][0])
        print("")

    (build_dir / "source-manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    (build_dir / "results.json").write_text(
        json.dumps(results, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )

    print("===== SUMMARY =====")
    failures = 0
    for result in results:
        integrated = " integrated" if result.get("integrated") else ""
        print(f"{result['name']:<12} {result['status']}{integrated}")
        if result["status"] != "COMPILE_PASS":
            failures += 1

    print("")
    print(f"Full compiler logs: {log_root}")
    print(f"Machine-readable results: {build_dir / 'results.json'}")
    print(f"Downloaded source hashes: {build_dir / 'source-manifest.json'}")

    if args.strict and failures:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
