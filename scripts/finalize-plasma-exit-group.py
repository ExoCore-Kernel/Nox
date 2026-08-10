#!/usr/bin/env python3
"""Implement Linux exit_group semantics for the cooperative Plasma runtime.

The pthread layer originally routed SYS_exit and SYS_exit_group through the same
single-task exit path.  That is incorrect once CLONE_THREAD is live: exit(2)
terminates only the calling task, while exit_group(2) terminates every task in
the caller's thread group.

Leaving sibling scheduler slots alive after a Qt/KDE process called exit_group
was especially destructive because those slots still referenced the exiting
leader's shared VM/CLONE_FILES state.  A parent could then reap the leader while
an orphaned sibling remained runnable, letting the scheduler resume a task whose
thread-group owner had already become a zombie (or whose address space had been
reclaimed).

This finalizer gives exit_group a real group teardown path:
* identify the TGID leader;
* clear each CLONE_CHILD_CLEARTID word and issue the required futex wake;
* remove every non-leader thread in the group from the scheduler immediately;
* make the leader the canonical exiting task even when a worker called
  exit_group;
* run the existing whole-process exit path exactly once so wait4, pipe/eventfd
  lifetime, and zombie/reaping logic remain centralized;
* keep raw SYS_exit as a calling-task exit.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(
            f"expected exit-group fragment not found ({found}): {old[:180]!r}"
        )
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    helper = r'''static int64_t plasma_process_exit_group(int status) {
    plasma_scheduler_init();
    struct plasma_process *caller = plasma_current_process();
    if (caller == 0) return plasma_process_exit(status);

    const int tgid = caller->tgid > 0 ? caller->tgid : caller->pid;
    size_t leader_slot = PLASMA_MAX_PROCESSES;
    struct plasma_process *leader = 0;

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *candidate = &plasma_processes[i];
        if (candidate->state == PLASMA_PROC_FREE || candidate->is_thread)
            continue;
        if (candidate->pid == tgid) {
            leader = candidate;
            leader_slot = i;
            break;
        }
    }

    /* A malformed/early task with no discoverable leader still gets normal
     * single-task exit semantics instead of fabricating a successful teardown. */
    if (leader == 0 || leader_slot >= PLASMA_MAX_PROCESSES)
        return plasma_process_exit(status);

    uint32_t terminated_threads = 0;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *member = &plasma_processes[i];
        if (member == leader || member->state == PLASMA_PROC_FREE ||
            !member->is_thread || member->tgid != tgid)
            continue;

        const uint64_t clear_tid = member->clear_tid_address;
        if (clear_tid != 0) {
            (void)user_store_u32(clear_tid, 0);
            (void)plasma_futex_wake(clear_tid, 1u);
        }

        /* Do not destroy member->image: CLONE_VM means it names the leader's
         * address space.  Marking the slot FREE removes it from every scheduler,
         * futex, poll and lifetime scan.  The normal clone path fully resets a
         * slot before reuse (and the performance pass makes that reset cheap). */
        member->state = PLASMA_PROC_FREE;
        member->pid = 0;
        member->tgid = 0;
        member->ppid = 0;
        member->is_thread = false;
        member->clear_tid_address = 0;
        member->blocked_futex_address = 0;
        member->blocked_futex_expected = 0;
        member->blocked_futex_has_deadline = false;
        member->blocked_futex_deadline_us = 0;
        member->pending_result = 0;
        ++terminated_threads;
    }

    /* exit_group may be issued by any thread.  The process parent waits for the
     * TGID leader, so make that slot canonical before invoking the existing
     * whole-process zombie/reaping path.  The caller and leader share the same
     * VM; no userspace return is made to the discarded caller. */
    plasma_current_slot = leader_slot;
    leader->state = PLASMA_PROC_RUNNING;

    serial_write("[linux:thread] exit_group tgid=");
    serial_u64((uint64_t)tgid);
    serial_write(" terminated-siblings=");
    serial_u64((uint64_t)terminated_threads);
    serial_write("\n");

    return plasma_process_exit(status);
}

'''
    text = rep(text, exit_anchor, helper + exit_anchor)

    old_dispatch = (
        "    case SYS_EXIT:\n"
        "    case SYS_EXIT_GROUP:\n"
        "        return plasma_process_exit((int)(a1 & 0xffu));\n"
    )
    new_dispatch = (
        "    case SYS_EXIT:\n"
        "        return plasma_process_exit((int)(a1 & 0xffu));\n"
        "    case SYS_EXIT_GROUP:\n"
        "        return plasma_process_exit_group((int)(a1 & 0xffu));\n"
    )
    text = rep(text, old_dispatch, new_dispatch)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized real SYS_exit_group thread-group teardown: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
