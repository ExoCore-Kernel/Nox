#!/usr/bin/env python3
"""Finalize zombie lifecycle for the cooperative Plasma process scheduler."""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected scheduler fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "    int exit_status;\n    int wait_target;\n",
        "    int exit_status;\n    bool reap_pending;\n    int wait_target;\n",
    )

    text = replace_once(
        text,
        "        plasma_finish_wait(waiter, child);\n"
        "        bytes_copy(&image, &saved_image, sizeof(image));\n"
        "        return;\n",
        "        plasma_finish_wait(waiter, child);\n"
        "        child->reap_pending = true;\n"
        "        bytes_copy(&image, &saved_image, sizeof(image));\n"
        "        return;\n",
    )

    old_cleanup = r'''static void plasma_cleanup_reaped(void) {
    const vmm_space_t current_space = vmm_current_space();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state != PLASMA_PROC_ZOMBIE || process->image.space == current_space)
            continue;
        bool has_waiter = false;
        for (size_t j = 0; j < PLASMA_MAX_PROCESSES; ++j) {
            struct plasma_process *waiter = &plasma_processes[j];
            if ((waiter->state == PLASMA_PROC_BLOCKED_WAIT || waiter->state == PLASMA_PROC_RUNNABLE ||
                 waiter->state == PLASMA_PROC_RUNNING) &&
                waiter->ppid != process->pid && waiter->pending_result == process->pid) {
                has_waiter = true;
                break;
            }
        }
        if (!has_waiter) continue;
        plasma_destroy_detached_image(&process->image);
        bytes_zero(process, sizeof(*process));
    }
}
'''
    new_cleanup = r'''static void plasma_cleanup_reaped(void) {
    const vmm_space_t current_space = vmm_current_space();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state != PLASMA_PROC_ZOMBIE || !process->reap_pending ||
            process->image.space == current_space)
            continue;
        plasma_destroy_detached_image(&process->image);
        bytes_zero(process, sizeof(*process));
    }
}
'''
    text = replace_once(text, old_cleanup, new_cleanup)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized cooperative scheduler zombie lifecycle: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
