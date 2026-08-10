#!/usr/bin/env python3
"""Give cooperative Plasma futex waits real timeout/progress semantics.

The pthread bring-up originally ignored FUTEX_WAIT timeouts.  That can leave Qt
threads permanently blocked when the peer that would normally wake them exits
or when a condition-variable wait is intentionally timed.  This finalizer keeps
the existing cooperative scheduler, but records absolute deadlines, expires
waiters with ETIMEDOUT, and idles on HLT until the nearest deadline when every
runnable task is asleep.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected futex-progress fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "#define LINUX_ENOSYS     38\n",
        "#define LINUX_ENOSYS     38\n#define LINUX_ETIMEDOUT 110\n",
    )

    # timer_uptime_us() is backed by the real 1 kHz PIT clock in Twilight.
    text = rep(
        text,
        "extern void linux_exec_set_return_state(uint64_t instruction_pointer,\n",
        "extern uint64_t timer_uptime_us(void);\n"
        "extern void linux_exec_set_return_state(uint64_t instruction_pointer,\n",
    )

    text = rep(
        text,
        "    uint64_t blocked_futex_address;\n"
        "    uint32_t blocked_futex_expected;\n",
        "    uint64_t blocked_futex_address;\n"
        "    uint32_t blocked_futex_expected;\n"
        "    bool blocked_futex_has_deadline;\n"
        "    uint64_t blocked_futex_deadline_us;\n",
    )

    text = rep(
        text,
        "        waiter->blocked_futex_address = 0;\n"
        "        waiter->blocked_futex_expected = 0;\n"
        "        waiter->pending_result = 0;\n",
        "        waiter->blocked_futex_address = 0;\n"
        "        waiter->blocked_futex_expected = 0;\n"
        "        waiter->blocked_futex_has_deadline = false;\n"
        "        waiter->blocked_futex_deadline_us = 0;\n"
        "        waiter->pending_result = 0;\n",
    )

    old_futex_head = '''static int64_t plasma_futex(uint64_t address,
                            uint32_t operation,
                            uint32_t value,
                            uint64_t timeout_address) {
    (void)timeout_address; /* timeout accounting comes with timer preemption */
    const uint32_t command = operation & PLASMA_FUTEX_CMD_MASK;
'''
    new_futex_head = '''struct plasma_futex_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static int64_t plasma_futex(uint64_t address,
                            uint32_t operation,
                            uint32_t value,
                            uint64_t timeout_address) {
    const uint32_t command = operation & PLASMA_FUTEX_CMD_MASK;
'''
    text = rep(text, old_futex_head, new_futex_head)

    text = rep(
        text,
        "    current->blocked_futex_address = address;\n"
        "    current->blocked_futex_expected = value;\n"
        "    current->pending_result = 0;\n",
        "    current->blocked_futex_address = address;\n"
        "    current->blocked_futex_expected = value;\n"
        "    current->blocked_futex_has_deadline = false;\n"
        "    current->blocked_futex_deadline_us = 0;\n"
        "    if (timeout_address != 0) {\n"
        "        struct plasma_futex_timespec timeout;\n"
        "        if (!user_copy_in(&timeout, timeout_address, sizeof(timeout)))\n"
        "            return -LINUX_EFAULT;\n"
        "        if (timeout.tv_sec < 0 || timeout.tv_nsec < 0 || timeout.tv_nsec >= 1000000000ll)\n"
        "            return -LINUX_EINVAL;\n"
        "        uint64_t delta_us = (uint64_t)timeout.tv_sec * 1000000ull;\n"
        "        delta_us += ((uint64_t)timeout.tv_nsec + 999ull) / 1000ull;\n"
        "        current->blocked_futex_has_deadline = true;\n"
        "        current->blocked_futex_deadline_us = timer_uptime_us() + delta_us;\n"
        "    }\n"
        "    current->pending_result = 0;\n",
    )

    # Add timeout expiry and a true cooperative idle path.  This is deliberately
    # scheduler-level rather than a fake successful syscall: a waiter remains
    # asleep until either FUTEX_WAKE or its actual PIT-backed deadline fires.
    pick_anchor = "static size_t plasma_pick_next(void) {\n"
    helpers = r'''static size_t plasma_expire_futex_timeouts(void) {
    const uint64_t now = timer_uptime_us();
    size_t expired = 0;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *waiter = &plasma_processes[i];
        if (waiter->state != PLASMA_PROC_BLOCKED_FUTEX ||
            !waiter->blocked_futex_has_deadline ||
            now < waiter->blocked_futex_deadline_us)
            continue;
        waiter->blocked_futex_address = 0;
        waiter->blocked_futex_expected = 0;
        waiter->blocked_futex_has_deadline = false;
        waiter->blocked_futex_deadline_us = 0;
        waiter->pending_result = -LINUX_ETIMEDOUT;
        waiter->state = PLASMA_PROC_RUNNABLE;
        ++expired;
    }
    return expired;
}

static bool plasma_nearest_futex_deadline(uint64_t *deadline_out) {
    bool found = false;
    uint64_t nearest = 0;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        const struct plasma_process *waiter = &plasma_processes[i];
        if (waiter->state != PLASMA_PROC_BLOCKED_FUTEX ||
            !waiter->blocked_futex_has_deadline)
            continue;
        if (!found || waiter->blocked_futex_deadline_us < nearest) {
            nearest = waiter->blocked_futex_deadline_us;
            found = true;
        }
    }
    if (found && deadline_out != 0) *deadline_out = nearest;
    return found;
}

'''
    text = rep(text, pick_anchor, helpers + pick_anchor)

    text = rep(
        text,
        "static size_t plasma_pick_next(void) {\n"
        "    for (size_t step = 1; step <= PLASMA_MAX_PROCESSES; ++step) {\n",
        "static size_t plasma_pick_next(void) {\n"
        "    (void)plasma_expire_futex_timeouts();\n"
        "retry_scan:\n"
        "    for (size_t step = 1; step <= PLASMA_MAX_PROCESSES; ++step) {\n",
    )

    text = rep(
        text,
        "    if (plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNABLE ||\n"
        "        plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNING)\n"
        "        return plasma_current_slot;\n"
        "    return PLASMA_MAX_PROCESSES;\n"
        "}\n",
        "    if (plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNABLE ||\n"
        "        plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNING)\n"
        "        return plasma_current_slot;\n"
        "\n"
        "    uint64_t deadline = 0;\n"
        "    if (plasma_nearest_futex_deadline(&deadline)) {\n"
        "        while (timer_uptime_us() < deadline)\n"
        "            __asm__ volatile (\"hlt\");\n"
        "        (void)plasma_expire_futex_timeouts();\n"
        "        goto retry_scan;\n"
        "    }\n"
        "    return PLASMA_MAX_PROCESSES;\n"
        "}\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized PIT-backed futex timeout/progress semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
