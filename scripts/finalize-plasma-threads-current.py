#!/usr/bin/env python3
"""Compatibility wrapper for the pthread finalizer after pipe-EOF transforms.

finalize-plasma-pipe-eof.py inserts last-writer bookkeeping between the
clear_child_tid store and process->exit_status.  The first pthread finalizer
matched the older contiguous process-exit fragment.  Intercept only that one
replacement and hook the thread-exit path immediately after plasma_save_active;
all other pthread transformations still come from finalize-plasma-threads.py.
"""
from __future__ import annotations

import importlib.util
import pathlib
import sys


def main() -> int:
    implementation = pathlib.Path(__file__).with_name("finalize-plasma-threads.py")
    spec = importlib.util.spec_from_file_location("plasma_threads_impl", implementation)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load pthread finalizer implementation")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    original_rep = module.rep

    def compatible_rep(text: str, old: str, new: str, count: int = 1) -> str:
        if (
            "plasma_save_active(process);" in old
            and "process->exit_status = status & 0xff;" in old
            and "if (process->is_thread)" in new
        ):
            prefix = (
                "    plasma_save_active(process);\n"
                "    if (process->clear_tid_address != 0)\n"
                "        (void)user_store_u32(process->clear_tid_address, 0);\n"
            )
            replacement = (
                "    plasma_save_active(process);\n"
                "    if (process->is_thread) {\n"
                "        const int tid = process->pid;\n"
                "        const uint64_t clear_tid = process->clear_tid_address;\n"
                "        if (clear_tid != 0) {\n"
                "            (void)user_store_u32(clear_tid, 0);\n"
                "            (void)plasma_futex_wake(clear_tid, 1u);\n"
                "        }\n"
                "        serial_write(\"[linux:thread] exited tid=\");\n"
                "        serial_u64((uint64_t)tid);\n"
                "        serial_write(\"\\n\");\n"
                "        bytes_zero(process, sizeof(*process));\n"
                "        return 0;\n"
                "    }\n"
                "    if (process->clear_tid_address != 0)\n"
                "        (void)user_store_u32(process->clear_tid_address, 0);\n"
            )
            return original_rep(text, prefix, replacement, count)
        return original_rep(text, old, new, count)

    module.rep = compatible_rep
    return int(module.main())


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
