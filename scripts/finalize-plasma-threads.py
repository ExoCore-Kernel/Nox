#!/usr/bin/env python3
"""Add the first real shared-VM pthread/futex ABI for Plasma/Qt bring-up.

Qt has now crossed the X11 handshake and reaches QThread::start().  Alpine uses
musl, whose pthread_create() calls clone(2) with the usual Linux thread-sharing
flags (CLONE_VM/FS/FILES/SIGHAND/THREAD/SYSVSEM/SETTLS, parent TID, and child
clear-TID).  The earlier Nox process scheduler intentionally rejected those
flags because every clone owned a private copied address space.

This finalizer adds the smallest cooperative pthread model needed for the GUI:

* clone(CLONE_THREAD...) creates another scheduler slot sharing the leader's
  VMM address space instead of copying physical pages;
* each thread keeps an independent register context, userspace stack and FS/TLS
  base while sharing VM, cwd and descriptor state at syscall boundaries;
* getpid returns the thread-group id while gettid remains unique per thread;
* FUTEX_WAIT/FUTEX_WAKE (including PRIVATE) suspend and wake scheduler slots;
* CLONE_CHILD_CLEARTID clears the musl TID word and performs the matching futex
  wake when a thread exits;
* sched_getaffinity reports the single cooperative CPU used by this bring-up.

This is deliberately not the final POSIX thread implementation: there is no
preemption, robust-futex recovery, PI futexes, signals-per-thread or clone3 yet.
It is enough to let musl pthreads make progress at syscall boundaries without
copying the process address space.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected pthread fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # Linux x86_64 flags used by musl pthread_create().
    text = rep(
        text,
        "#define CLONE_THREAD         0x00010000ull\n"
        "#define CLONE_PARENT_SETTID  0x00100000ull\n",
        "#define CLONE_THREAD         0x00010000ull\n"
        "#define CLONE_SYSVSEM        0x00040000ull\n"
        "#define CLONE_SETTLS         0x00080000ull\n"
        "#define CLONE_PARENT_SETTID  0x00100000ull\n",
    )
    text = rep(
        text,
        "#define CLONE_CHILD_CLEARTID 0x00200000ull\n"
        "#define CLONE_CHILD_SETTID   0x01000000ull\n",
        "#define CLONE_CHILD_CLEARTID 0x00200000ull\n"
        "#define CLONE_DETACHED       0x00400000ull\n"
        "#define CLONE_CHILD_SETTID   0x01000000ull\n",
    )

    # sched_getaffinity is queried by Qt when deciding how many worker threads
    # it may create.  The bring-up scheduler is intentionally one CPU.
    text = rep(
        text,
        "#define SYS_FUTEX          202ull\n",
        "#define SYS_FUTEX          202ull\n"
        "#define SYS_SCHED_GETAFFINITY 204ull\n",
    )

    # A blocked futex is a scheduler wait state just like blocked pipe/socket
    # I/O.  The X11 poll finalizer is already applied before this one.
    text = rep(
        text,
        "    PLASMA_PROC_BLOCKED_POLL,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
        "    PLASMA_PROC_BLOCKED_POLL,\n"
        "    PLASMA_PROC_BLOCKED_FUTEX,\n"
        "    PLASMA_PROC_ZOMBIE,\n",
    )

    # Track Linux thread-group identity separately from the scheduler TID.
    text = rep(
        text,
        "    int pid;\n"
        "    int ppid;\n",
        "    int pid;\n"
        "    int tgid;\n"
        "    int ppid;\n"
        "    bool is_thread;\n",
    )

    # Futex wait identity lives beside the other cooperative-wait fields.
    text = rep(
        text,
        "    uint16_t blocked_poll_socket_object;\n"
        "    uint64_t blocked_poll_address;\n"
        "    uint64_t blocked_io_address;\n",
        "    uint16_t blocked_poll_socket_object;\n"
        "    uint64_t blocked_poll_address;\n"
        "    uint64_t blocked_futex_address;\n"
        "    uint32_t blocked_futex_expected;\n"
        "    uint64_t blocked_io_address;\n",
    )

    # Init and ordinary fork become thread-group leaders.
    text = rep(
        text,
        "    init->pid = PLASMA_INIT_PID;\n"
        "    init->ppid = 0;\n",
        "    init->pid = PLASMA_INIT_PID;\n"
        "    init->tgid = PLASMA_INIT_PID;\n"
        "    init->ppid = 0;\n",
    )
    text = rep(
        text,
        "    child->pid = plasma_next_pid++;\n"
        "    child->ppid = parent->pid;\n",
        "    child->pid = plasma_next_pid++;\n"
        "    child->tgid = child->pid;\n"
        "    child->ppid = parent->pid;\n",
    )

    # Threads are not wait4/waitid children.  They are joined by musl through
    # the clear-TID futex instead.
    text = rep(
        text,
        "static bool plasma_wait_matches(int requested_pid, const struct plasma_process *child, int parent_pid) {\n"
        "    if (!plasma_is_child_of(child, parent_pid)) return false;\n",
        "static bool plasma_wait_matches(int requested_pid, const struct plasma_process *child, int parent_pid) {\n"
        "    if (child != 0 && child->is_thread) return false;\n"
        "    if (!plasma_is_child_of(child, parent_pid)) return false;\n",
    )

    # The existing process struct embeds a large shell_image.  Keep the group
    # leader's copy canonical and refresh/sync thread copies at scheduler
    # boundaries.  This is intentionally simple while the process model is
    # still cooperative; a later kernel can replace it with a shared mm object.
    save_anchor = "static void plasma_save_active(struct plasma_process *process) {\n"
    shared_helpers = r'''static struct plasma_process *plasma_thread_group_leader(struct plasma_process *process) {
    if (process == 0 || process->tgid <= 0) return process;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *candidate = &plasma_processes[i];
        if (candidate->state == PLASMA_PROC_FREE) continue;
        if (candidate->pid == process->tgid && !candidate->is_thread) return candidate;
    }
    return process;
}

static void plasma_thread_refresh_shared(struct plasma_process *process) {
    if (process == 0 || !process->is_thread) return;
    struct plasma_process *leader = plasma_thread_group_leader(process);
    if (leader == 0 || leader == process) return;
    bytes_copy(&process->image, &leader->image, sizeof(process->image));
    bytes_copy(process->files, leader->files, sizeof(process->files));
    bytes_copy(process->runtime_fds, leader->runtime_fds, sizeof(process->runtime_fds));
    bytes_copy(process->low_runtime_fds, leader->low_runtime_fds, sizeof(process->low_runtime_fds));
    plasma_copy_c_string(process->cwd, sizeof(process->cwd), leader->cwd);
    plasma_copy_c_string(process->exec_path, sizeof(process->exec_path), leader->exec_path);
    process->foreground_pgrp = leader->foreground_pgrp;
}

static void plasma_thread_sync_shared(struct plasma_process *process) {
    if (process == 0 || !process->is_thread) return;
    struct plasma_process *leader = plasma_thread_group_leader(process);
    if (leader == 0 || leader == process) return;
    bytes_copy(&leader->image, &process->image, sizeof(leader->image));
    bytes_copy(leader->files, process->files, sizeof(leader->files));
    bytes_copy(leader->runtime_fds, process->runtime_fds, sizeof(leader->runtime_fds));
    bytes_copy(leader->low_runtime_fds, process->low_runtime_fds, sizeof(leader->low_runtime_fds));
    plasma_copy_c_string(leader->cwd, sizeof(leader->cwd), process->cwd);
    plasma_copy_c_string(leader->exec_path, sizeof(leader->exec_path), process->exec_path);
    leader->foreground_pgrp = process->foreground_pgrp;
}

'''
    text = rep(text, save_anchor, shared_helpers + save_anchor)

    text = rep(
        text,
        "    process->foreground_pgrp = foreground_pgrp;\n"
        "}\n\n"
        "static void plasma_load_active(struct plasma_process *process) {\n",
        "    process->foreground_pgrp = foreground_pgrp;\n"
        "    plasma_thread_sync_shared(process);\n"
        "}\n\n"
        "static void plasma_load_active(struct plasma_process *process) {\n",
    )
    text = rep(
        text,
        "static void plasma_load_active(struct plasma_process *process) {\n"
        "    if (process == 0) return;\n"
        "    bytes_copy(&image, &process->image, sizeof(image));\n",
        "static void plasma_load_active(struct plasma_process *process) {\n"
        "    if (process == 0) return;\n"
        "    plasma_thread_refresh_shared(process);\n"
        "    bytes_copy(&image, &process->image, sizeof(image));\n",
    )

    # The thread clone and futex helpers live after all scheduler primitives and
    # before process-exit, so they can use the current process table directly.
    exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    helpers = r'''#define PLASMA_FUTEX_WAIT 0u
#define PLASMA_FUTEX_WAKE 1u
#define PLASMA_FUTEX_PRIVATE_FLAG 128u
#define PLASMA_FUTEX_CLOCK_REALTIME 256u
#define PLASMA_FUTEX_CMD_MASK 0x7fu

static int64_t plasma_futex_wake(uint64_t address, uint32_t count) {
    if (address == 0 || (address & 3u) != 0) return -LINUX_EINVAL;
    if (count == 0) return 0;

    uint32_t woke = 0;
    const vmm_space_t shared_space = image.space;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES && woke < count; ++i) {
        struct plasma_process *waiter = &plasma_processes[i];
        if (waiter->state != PLASMA_PROC_BLOCKED_FUTEX ||
            waiter->blocked_futex_address != address ||
            waiter->image.space != shared_space)
            continue;
        waiter->blocked_futex_address = 0;
        waiter->blocked_futex_expected = 0;
        waiter->pending_result = 0;
        waiter->state = PLASMA_PROC_RUNNABLE;
        ++woke;
    }
    if (woke != 0) {
        serial_write("[linux:futex] wake address=");
        serial_u64(address);
        serial_write(" count=");
        serial_u64(woke);
        serial_write("\n");
    }
    return (int64_t)woke;
}

static int64_t plasma_futex(uint64_t address,
                            uint32_t operation,
                            uint32_t value,
                            uint64_t timeout_address) {
    (void)timeout_address; /* timeout accounting comes with timer preemption */
    const uint32_t command = operation & PLASMA_FUTEX_CMD_MASK;
    if (command == PLASMA_FUTEX_WAKE)
        return plasma_futex_wake(address, value);
    if (command != PLASMA_FUTEX_WAIT) return -LINUX_ENOSYS;
    if (address == 0 || (address & 3u) != 0) return -LINUX_EINVAL;

    uint32_t current_value = 0;
    if (!user_copy_in(&current_value, address, sizeof(current_value))) return -LINUX_EFAULT;
    if (current_value != value) return -LINUX_EAGAIN;

    struct plasma_process *current = plasma_current_process();
    if (current == 0 || current->state != PLASMA_PROC_RUNNING) return -LINUX_EAGAIN;
    plasma_save_active(current);
    current->blocked_futex_address = address;
    current->blocked_futex_expected = value;
    current->pending_result = 0;
    current->state = PLASMA_PROC_BLOCKED_FUTEX;

    serial_write("[linux:futex] wait tid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" address=");
    serial_u64(address);
    serial_write(" expected=");
    serial_u64((uint64_t)value);
    serial_write("\n");
    return 0;
}

static int64_t plasma_clone_thread(uint64_t flags,
                                   uint64_t child_stack,
                                   uint64_t parent_tid_address,
                                   uint64_t child_tid_address,
                                   uint64_t tls_address) {
    plasma_scheduler_init();
    struct plasma_process *parent = plasma_current_process();
    if (parent == 0) return -LINUX_EAGAIN;

    serial_write("[linux:thread] clone request flags=");
    serial_u64(flags);
    serial_write(" stack=");
    serial_u64(child_stack);
    serial_write(" tls=");
    serial_u64(tls_address);
    serial_write("\n");

    const uint64_t required = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
                              CLONE_THREAD | CLONE_SYSVSEM;
    const uint64_t allowed = required | CLONE_SETTLS | CLONE_PARENT_SETTID |
                             CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID |
                             CLONE_DETACHED;
    if ((flags & required) != required || (flags & ~allowed) != 0 ||
        (flags & 0xffull) != 0)
        return -LINUX_ENOSYS;
    if (child_stack == 0) return -LINUX_EINVAL;
    if ((flags & CLONE_SETTLS) != 0 && tls_address == 0) return -LINUX_EINVAL;

    const int free_slot = plasma_find_free_slot();
    if (free_slot < 0) return -LINUX_EAGAIN;
    const int tid = plasma_next_pid++;

    if ((flags & CLONE_PARENT_SETTID) != 0 && parent_tid_address != 0 &&
        !user_store_u32(parent_tid_address, (uint32_t)tid))
        return -LINUX_EFAULT;
    if ((flags & CLONE_CHILD_SETTID) != 0 && child_tid_address != 0 &&
        !user_store_u32(child_tid_address, (uint32_t)tid))
        return -LINUX_EFAULT;

    struct plasma_process *child = &plasma_processes[free_slot];
    bytes_zero(child, sizeof(*child));
    child->state = PLASMA_PROC_RUNNABLE;
    child->pid = tid;
    child->tgid = parent->tgid > 0 ? parent->tgid : parent->pid;
    child->ppid = parent->ppid;
    child->is_thread = true;
    child->pending_result = 0;
    child->clear_tid_address = (flags & CLONE_CHILD_CLEARTID) != 0 ? child_tid_address : 0;
    child->fs_base = (flags & CLONE_SETTLS) != 0 ? tls_address : fs_base;
    child->foreground_pgrp = foreground_pgrp;

    /* CLONE_VM/FILES/FS: all these snapshots name the same underlying runtime
     * objects and are resynchronized with the group leader at each scheduler
     * boundary.  Crucially, no physical user pages are copied here. */
    bytes_copy(&child->image, &image, sizeof(child->image));
    bytes_copy(child->files, rootfs_open_files, sizeof(rootfs_open_files));
    bytes_copy(child->runtime_fds, plasma_runtime_fds, sizeof(plasma_runtime_fds));
    bytes_copy(child->low_runtime_fds, plasma_low_runtime_fds, sizeof(plasma_low_runtime_fds));
    plasma_copy_c_string(child->cwd, sizeof(child->cwd), current_directory);
    plasma_copy_c_string(child->exec_path, sizeof(child->exec_path), plasma_exec_path);

    linux_process_capture_context(child->context);
    child->context[1] = child_stack;

    serial_write("[linux:thread] clone runnable tid=");
    serial_u64((uint64_t)child->pid);
    serial_write(" tgid=");
    serial_u64((uint64_t)child->tgid);
    serial_write(" shared-space=");
    serial_u64((uint64_t)child->image.space);
    serial_write("\n");
    return tid;
}

'''
    text = rep(text, exit_anchor, helpers + exit_anchor)

    # Thread exit is not a waitable process zombie.  Clear the musl TID word,
    # wake joiners, preserve the leader's shared VM metadata, and immediately
    # release the scheduler slot without destroying the shared address space.
    text = rep(
        text,
        "    plasma_save_active(process);\n"
        "    if (process->clear_tid_address != 0)\n"
        "        (void)user_store_u32(process->clear_tid_address, 0);\n"
        "    process->exit_status = status & 0xff;\n",
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
        "    process->exit_status = status & 0xff;\n",
    )

    # Raw clone arg5 is the x86_64 TLS pointer.  Keep the existing private-VM
    # fork/clone implementation for non-thread clone calls.
    text = rep(
        text,
        "    case SYS_CLONE:\n"
        "        return plasma_fork_process(a1, a2, a3, a4, true);\n",
        "    case SYS_CLONE:\n"
        "        if ((a1 & CLONE_THREAD) != 0)\n"
        "            return plasma_clone_thread(a1, a2, a3, a4, a5);\n"
        "        return plasma_fork_process(a1, a2, a3, a4, true);\n",
    )

    # Linux exposes PID and TID separately once a process is multithreaded.
    old_ids = '''    case SYS_GETPID:
    case SYS_GETTID: {
        plasma_scheduler_init();
        struct plasma_process *process = plasma_current_process();
        return process != 0 ? process->pid : PLASMA_INIT_PID;
    }
'''
    new_ids = '''    case SYS_GETPID: {
        plasma_scheduler_init();
        struct plasma_process *process = plasma_current_process();
        if (process == 0) return PLASMA_INIT_PID;
        return process->tgid > 0 ? process->tgid : process->pid;
    }
    case SYS_GETTID: {
        plasma_scheduler_init();
        struct plasma_process *process = plasma_current_process();
        return process != 0 ? process->pid : PLASMA_INIT_PID;
    }
'''
    text = rep(text, old_ids, new_ids)

    # Replace the old fake-success futex with scheduler-backed WAIT/WAKE and
    # expose a one-CPU affinity mask to Qt.
    text = rep(
        text,
        "    case SYS_FUTEX: return 0;\n",
        "    case SYS_FUTEX:\n"
        "        return plasma_futex(a1, (uint32_t)a2, (uint32_t)a3, a4);\n"
        "    case SYS_SCHED_GETAFFINITY:\n"
        "        if (a2 < 8u) return -LINUX_EINVAL;\n"
        "        if (!user_zero(a3, a2) || !user_store_u64(a3, 1ull)) return -LINUX_EFAULT;\n"
        "        return 8;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Finalized cooperative musl pthread clone + futex ABI: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
