#!/usr/bin/env python3
"""Turn CPL3 CPU faults into Linux task death instead of a Twilight panic.

The x86 exception stubs historically panic unconditionally.  That is correct for
kernel faults, but a #GP/#PF/#UD raised by a Linux process must not halt the
whole kernel.  irq_stubs.S now calls the exported handler added here only when a
fault came from CPL3; the assembly still panics if this Plasma scheduler is not
active.

Fatal exceptions terminate the whole current thread group, not just the faulting
pthread.  The existing process-exit path then performs pipe EOF/resource cleanup,
wakes the parent, and the cooperative scheduler selects another saved userspace
context.  The assembly return helper resumes that selected context through the
same CR3/register/SYSRET path used after a syscall.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected user-exception fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Existing fatal-signal helper is earlier in the generated file than the
    # pthread-aware group-termination implementation below.
    text = rep(text,
               "static int64_t plasma_kde_thread_signal(int tid, int signal) {\n",
               "static int64_t plasma_terminate_current_thread_group(int signal);\n\n"
               "static int64_t plasma_kde_thread_signal(int tid, int signal) {\n")
    text = rep(text,
               "        return plasma_process_exit(128 + signal);\n",
               "        return plasma_terminate_current_thread_group(signal);\n")

    dispatch_anchor = "static int64_t shell_dispatch(uint64_t number,\n"
    helpers = r'''#define PLASMA_EXCEPTION_UNHANDLED (LINUX_EXIT_SENTINEL + 1)

static int plasma_exception_signal(uint64_t vector) {
    switch (vector) {
    case 0:                         /* #DE divide error */
    case 16:                        /* #MF x87 FP */
    case 19:                        /* #XM SIMD FP */
        return PLASMA_SIGFPE;
    case 1:                         /* #DB */
    case 3:                         /* #BP */
        return 5;                   /* SIGTRAP */
    case 6:                         /* #UD */
        return PLASMA_SIGILL;
    case 17:                        /* #AC */
        return 7;                   /* SIGBUS */
    case 13:                        /* #GP */
    case 14:                        /* #PF */
    default:
        return PLASMA_SIGSEGV;
    }
}

static int64_t plasma_terminate_current_thread_group(int signal) {
    plasma_scheduler_init();
    struct plasma_process *current = plasma_current_process();
    if (current == 0) return LINUX_EXIT_SENTINEL;

    struct plasma_process *leader = plasma_thread_group_leader(current);
    if (leader == 0) leader = current;
    const int tgid = leader->tgid > 0 ? leader->tgid : leader->pid;

    size_t leader_slot = PLASMA_MAX_PROCESSES;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        if (&plasma_processes[i] == leader) {
            leader_slot = i;
            break;
        }
    }
    if (leader_slot >= PLASMA_MAX_PROCESSES)
        return plasma_process_exit(128 + signal);

    /* exit_group semantics: every pthread in the group stops.  The shared VM
     * belongs to the leader and is deliberately not destroyed from a thread
     * slot. Clear CHILD_CLEARTID words before releasing those scheduler slots. */
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *member = &plasma_processes[i];
        if (member == leader || member->state == PLASMA_PROC_FREE ||
            !member->is_thread || member->tgid != tgid)
            continue;
        if (member->clear_tid_address != 0) {
            (void)user_store_u32(member->clear_tid_address, 0);
            (void)plasma_futex_wake(member->clear_tid_address, UINT32_MAX);
        }
        bytes_zero(member, sizeof(*member));
    }

    /* The fault may have occurred in a non-leader thread. Point the scheduler at
     * the actual process leader before invoking the normal process exit path so
     * parent/wait/descriptor lifetime semantics remain process-scoped. */
    plasma_current_slot = leader_slot;
    return plasma_process_exit(128 + signal);
}

/* Called only by the CPL3 branch of irq_stubs.S.  Returning the special
 * UNHANDLED value asks the assembly stub to preserve its old panic behavior,
 * which is important for early user-mode self-tests before Plasma owns CPL3. */
int64_t linux_busybox_user_exception(uint64_t vector,
                                     uint64_t error_code,
                                     uint64_t fault_rip) {
    if (!shell_active || !plasma_scheduler_ready)
        return PLASMA_EXCEPTION_UNHANDLED;

    struct plasma_process *faulting = plasma_current_process();
    if (faulting == 0 || faulting->state == PLASMA_PROC_FREE)
        return PLASMA_EXCEPTION_UNHANDLED;

    const int signal = plasma_exception_signal(vector);
    serial_write("[linux:process] userspace CPU exception pid=");
    serial_u64((uint64_t)faulting->pid);
    serial_write(" vector=");
    serial_u64(vector);
    serial_write(" error=");
    serial_u64(error_code);
    serial_write(" rip=");
    serial_u64(fault_rip);
    serial_write(" -> signal=");
    serial_u64((uint64_t)signal);
    serial_write("\n");

    const int64_t exit_result = plasma_terminate_current_thread_group(signal);
    if (exit_result == LINUX_EXIT_SENTINEL) return exit_result;

    /* The process leader is now a zombie. Select and load another runnable task;
     * plasma_load_active() fills the same linux_saved_* state used by SYSRET. */
    return plasma_schedule_after_syscall(exit_result);
}

'''
    text = rep(text, dispatch_anchor, helpers + dispatch_anchor)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized CPL3 exception -> thread-group death + scheduler resume ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
