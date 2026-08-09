#!/usr/bin/env python3
"""Make post-teardown Plasma execve failures return safely to the kernel."""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected execve fragment not found: {old[:100]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    helper = r'''static int64_t plasma_exec_abandon(const char *reason) {
    serial_write("[linux:plasma] execve replacement failed after old image teardown: ");
    serial_write(reason != 0 ? reason : "unknown");
    serial_write("\n");
    shell_exit_status = 126;
    shell_exit_seen = true;
    write_msr(IA32_FS_BASE_MSR, 0);
    fs_base = 0;
    return LINUX_EXIT_SENTINEL;
}

'''
    text = replace_once(text, "static int64_t plasma_execve(uint64_t filename_address,\n",
                        helper + "static int64_t plasma_execve(uint64_t filename_address,\n")

    text = replace_once(
        text,
        "    if (!plasma_map_elf(&executable, PLASMA_MAIN_BIAS, &main_elf)) return -LINUX_ENOEXEC;\n",
        "    if (!plasma_map_elf(&executable, PLASMA_MAIN_BIAS, &main_elf))\n"
        "        return plasma_exec_abandon(\"main ELF mapping\");\n",
    )
    text = replace_once(
        text,
        "        if (!plasma_map_elf(&interpreter, PLASMA_INTERP_BIAS, &interp_elf))\n"
        "            return -LINUX_ENOEXEC;\n",
        "        if (!plasma_map_elf(&interpreter, PLASMA_INTERP_BIAS, &interp_elf))\n"
        "            return plasma_exec_abandon(\"PT_INTERP mapping\");\n",
    )
    text = replace_once(
        text,
        "    if (!align_up(main_elf.max_end, &image.brk_base)) return -LINUX_ENOMEM;\n",
        "    if (!align_up(main_elf.max_end, &image.brk_base))\n"
        "        return plasma_exec_abandon(\"brk setup\");\n",
    )
    text = replace_once(
        text,
        "    if (!plasma_build_exec_stack(&main_elf, interp_base, filename)) return -LINUX_ENOMEM;\n",
        "    if (!plasma_build_exec_stack(&main_elf, interp_base, filename))\n"
        "        return plasma_exec_abandon(\"initial stack\");\n",
    )
    text = replace_once(
        text,
        "    for (size_t i = 0; i < image.page_count; ++i)\n"
        "        if (!protect_page(&image.pages[i])) return -LINUX_EACCES;\n",
        "    for (size_t i = 0; i < image.page_count; ++i)\n"
        "        if (!protect_page(&image.pages[i]))\n"
        "            return plasma_exec_abandon(\"final page protections\");\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma execve failure handling: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
