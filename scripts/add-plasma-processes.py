#!/usr/bin/env python3
"""Add a cooperative multi-process scheduler to the Plasma bring-up ABI.

Processes have independent CR3s and complete syscall-boundary register contexts.
Scheduling is round-robin at Linux syscall boundaries.  This is deliberately the
smallest useful concurrent scheduler for Plasma bring-up: fork/clone can return
to both parent and child, wait4 can block, and unrelated processes can remain
alive together.  Timer preemption and CLONE_VM threads come later.
"""

from __future__ import annotations

import pathlib
import sys


def replace_once(text: str, old: str, new: str) -> str:
    if old not in text:
        raise RuntimeError(f"expected generated source fragment not found: {old[:140]!r}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = replace_once(
        text,
        "#define SYS_EXECVE          59ull\n",
        "#define SYS_CLONE           56ull\n"
        "#define SYS_FORK            57ull\n"
        "#define SYS_VFORK           58ull\n"
        "#define SYS_EXECVE          59ull\n",
    )

    exec_decl = (
        "extern void linux_exec_set_return_state(uint64_t instruction_pointer,\n"
        "                                        uint64_t stack_pointer);\n"
    )
    text = replace_once(
        text,
        exec_decl,
        exec_decl
        + "extern void linux_process_capture_context(uint64_t *words);\n"
        + "extern void linux_process_restore_context(const uint64_t *words);\n"
        + "extern void linux_process_set_return_space(uint64_t space);\n",
    )

    state_anchor = "static struct rootfs_open_file rootfs_open_files[ROOTFS_FD_COUNT];\n"
    state = state_anchor + r'''

#define PLASMA_MAX_PROCESSES 12u
#define PLASMA_CONTEXT_WORDS 15u
#define PLASMA_INIT_PID 1
#define PLASMA_SIGCHLD 17ull
#define PLASMA_WNOHANG 1ull

#define CLONE_VM             0x00000100ull
#define CLONE_FS             0x00000200ull
#define CLONE_FILES          0x00000400ull
#define CLONE_SIGHAND        0x00000800ull
#define CLONE_VFORK          0x00004000ull
#define CLONE_THREAD         0x00010000ull
#define CLONE_PARENT_SETTID  0x00100000ull
#define CLONE_CHILD_CLEARTID 0x00200000ull
#define CLONE_CHILD_SETTID   0x01000000ull
#define PLASMA_CLONE_UNSUPPORTED \
    (CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD)
#define PLASMA_CLONE_KNOWN \
    (0xffull | CLONE_VFORK | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | \
     CLONE_CHILD_SETTID | PLASMA_CLONE_UNSUPPORTED)

enum plasma_process_state {
    PLASMA_PROC_FREE = 0,
    PLASMA_PROC_RUNNABLE,
    PLASMA_PROC_RUNNING,
    PLASMA_PROC_BLOCKED_WAIT,
    PLASMA_PROC_ZOMBIE,
};

struct plasma_process {
    enum plasma_process_state state;
    int pid;
    int ppid;
    int exit_status;
    int wait_target;
    uint64_t wait_status_address;
    uint64_t wait_rusage_address;
    int64_t pending_result;
    uint64_t clear_tid_address;
    uint64_t fs_base;
    int foreground_pgrp;
    uint64_t context[PLASMA_CONTEXT_WORDS];
    struct shell_image image;
    struct rootfs_open_file files[ROOTFS_FD_COUNT];
    char cwd[64];
    char exec_path[PLASMA_EXEC_STRING];
};

static struct plasma_process plasma_processes[PLASMA_MAX_PROCESSES];
static size_t plasma_current_slot;
static int plasma_next_pid = 2;
static bool plasma_scheduler_ready;
static struct shell_image plasma_child_build_image;

static int64_t plasma_process_exit(int status);
'''
    text = replace_once(text, state_anchor, state)

    process_code = r'''
static void plasma_copy_c_string(char *destination, size_t capacity, const char *source) {
    if (destination == 0 || capacity == 0) return;
    size_t i = 0;
    if (source != 0) {
        while (i + 1u < capacity && source[i] != '\0') {
            destination[i] = source[i];
            ++i;
        }
    }
    destination[i] = '\0';
}

static struct plasma_process *plasma_current_process(void) {
    if (!plasma_scheduler_ready || plasma_current_slot >= PLASMA_MAX_PROCESSES)
        return 0;
    return &plasma_processes[plasma_current_slot];
}

static void plasma_destroy_detached_image(struct shell_image *victim) {
    if (victim == 0 || victim->space == VMM_INVALID_SPACE) return;
    if (victim->space == vmm_current_space()) return;
    for (size_t i = 0; i < victim->page_count; ++i) {
        uint64_t physical = 0;
        if (vmm_unmap_page(victim->space, victim->pages[i].va, &physical) && physical != 0)
            (void)pmm_free_page(physical);
    }
    (void)vmm_destroy_address_space(victim->space);
    bytes_zero(victim, sizeof(*victim));
}

static bool plasma_clone_image(const struct shell_image *parent,
                               struct shell_image *child) {
    if (parent == 0 || child == 0 || parent->space == VMM_INVALID_SPACE)
        return false;
    bytes_copy(child, parent, sizeof(*child));
    child->space = vmm_create_address_space();
    child->page_count = 0;
    if (child->space == VMM_INVALID_SPACE) return false;

    for (size_t i = 0; i < parent->page_count; ++i) {
        if (child->page_count >= SHELL_MAX_PAGES) goto fail;
        const struct shell_page *source_page = &parent->pages[i];
        const uint64_t physical = pmm_alloc_page();
        if (physical == 0) goto fail;
        void *destination = pmm_phys_to_virt(physical);
        const void *source = pmm_phys_to_virt(source_page->phys);
        if (destination == 0 || source == 0) {
            (void)pmm_free_page(physical);
            goto fail;
        }
        bytes_copy(destination, source, TWILIGHT_PAGE_SIZE);
        if (!vmm_map_page(child->space, source_page->va, physical, source_page->flags)) {
            (void)pmm_free_page(physical);
            goto fail;
        }
        child->pages[child->page_count].va = source_page->va;
        child->pages[child->page_count].phys = physical;
        child->pages[child->page_count].flags = source_page->flags;
        ++child->page_count;
    }
    return true;

fail:
    plasma_destroy_detached_image(child);
    return false;
}

static void plasma_save_active(struct plasma_process *process) {
    if (process == 0) return;
    linux_process_capture_context(process->context);
    bytes_copy(&process->image, &image, sizeof(image));
    bytes_copy(process->files, rootfs_open_files, sizeof(rootfs_open_files));
    plasma_copy_c_string(process->cwd, sizeof(process->cwd), current_directory);
    plasma_copy_c_string(process->exec_path, sizeof(process->exec_path), plasma_exec_path);
    process->fs_base = fs_base;
    process->foreground_pgrp = foreground_pgrp;
}

static void plasma_load_active(struct plasma_process *process) {
    if (process == 0) return;
    bytes_copy(&image, &process->image, sizeof(image));
    bytes_copy(rootfs_open_files, process->files, sizeof(rootfs_open_files));
    plasma_copy_c_string(current_directory, sizeof(current_directory), process->cwd);
    plasma_copy_c_string(plasma_exec_path, sizeof(plasma_exec_path), process->exec_path);
    fs_base = process->fs_base;
    foreground_pgrp = process->foreground_pgrp;
    write_msr(IA32_FS_BASE_MSR, fs_base);
    linux_process_restore_context(process->context);
    linux_process_set_return_space(image.space);
}

static void plasma_scheduler_init(void) {
    if (plasma_scheduler_ready) return;
    bytes_zero(plasma_processes, sizeof(plasma_processes));
    struct plasma_process *init = &plasma_processes[0];
    init->state = PLASMA_PROC_RUNNING;
    init->pid = PLASMA_INIT_PID;
    init->ppid = 0;
    init->pending_result = 0;
    plasma_current_slot = 0;
    plasma_scheduler_ready = true;
    plasma_save_active(init);
    serial_write("[linux:process] cooperative scheduler online; init pid=1\n");
}

static int plasma_find_free_slot(void) {
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i)
        if (plasma_processes[i].state == PLASMA_PROC_FREE) return (int)i;
    return -1;
}

static struct plasma_process *plasma_find_pid(int pid) {
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i)
        if (plasma_processes[i].state != PLASMA_PROC_FREE && plasma_processes[i].pid == pid)
            return &plasma_processes[i];
    return 0;
}

static bool plasma_is_child_of(const struct plasma_process *candidate, int parent_pid) {
    return candidate != 0 && candidate->state != PLASMA_PROC_FREE &&
           candidate->ppid == parent_pid;
}

static bool plasma_wait_matches(int requested_pid, const struct plasma_process *child, int parent_pid) {
    if (!plasma_is_child_of(child, parent_pid)) return false;
    if (requested_pid > 0) return child->pid == requested_pid;
    return true; /* -1/0/<-1 are sufficient as "any child" for this bring-up stage. */
}

static bool plasma_has_matching_live_child(int parent_pid, int requested_pid) {
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *child = &plasma_processes[i];
        if (!plasma_wait_matches(requested_pid, child, parent_pid)) continue;
        if (child->state != PLASMA_PROC_ZOMBIE) return true;
    }
    return false;
}

static void plasma_finish_wait(struct plasma_process *waiter,
                               struct plasma_process *child) {
    if (waiter == 0 || child == 0) return;
    if (waiter->wait_status_address != 0) {
        const uint32_t wait_status = (uint32_t)(child->exit_status & 0xff) << 8;
        (void)user_store_u32(waiter->wait_status_address, wait_status);
    }
    if (waiter->wait_rusage_address != 0)
        (void)user_zero(waiter->wait_rusage_address, 144);
    waiter->pending_result = child->pid;
    waiter->wait_target = 0;
    waiter->wait_status_address = 0;
    waiter->wait_rusage_address = 0;
    waiter->state = PLASMA_PROC_RUNNABLE;
}

static void plasma_wake_waiters_for(struct plasma_process *child) {
    if (child == 0) return;
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *waiter = &plasma_processes[i];
        if (waiter->state != PLASMA_PROC_BLOCKED_WAIT) continue;
        if (!plasma_wait_matches(waiter->wait_target, child, waiter->pid)) continue;
        /* Store status using the waiter's own address-space metadata. */
        struct shell_image saved_image;
        bytes_copy(&saved_image, &image, sizeof(image));
        bytes_copy(&image, &waiter->image, sizeof(image));
        plasma_finish_wait(waiter, child);
        bytes_copy(&image, &saved_image, sizeof(image));
        return;
    }
}

static void plasma_cleanup_reaped(void) {
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

static size_t plasma_pick_next(void) {
    for (size_t step = 1; step <= PLASMA_MAX_PROCESSES; ++step) {
        size_t slot = (plasma_current_slot + step) % PLASMA_MAX_PROCESSES;
        if (plasma_processes[slot].state == PLASMA_PROC_RUNNABLE)
            return slot;
    }
    if (plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNABLE ||
        plasma_processes[plasma_current_slot].state == PLASMA_PROC_RUNNING)
        return plasma_current_slot;
    return PLASMA_MAX_PROCESSES;
}

static int64_t plasma_schedule_after_syscall(int64_t result) {
    plasma_scheduler_init();
    struct plasma_process *current = plasma_current_process();
    if (current == 0) return result;

    if (result == LINUX_EXIT_SENTINEL) return result;

    if (current->state == PLASMA_PROC_RUNNING) {
        plasma_save_active(current);
        current->pending_result = result;
        current->state = PLASMA_PROC_RUNNABLE;
    }

    const size_t next_slot = plasma_pick_next();
    if (next_slot >= PLASMA_MAX_PROCESSES) {
        serial_write("[linux:process] scheduler has no runnable process\n");
        shell_exit_status = 125;
        shell_exit_seen = true;
        return LINUX_EXIT_SENTINEL;
    }

    plasma_current_slot = next_slot;
    struct plasma_process *next = &plasma_processes[next_slot];
    next->state = PLASMA_PROC_RUNNING;
    plasma_load_active(next);
    const int64_t selected_result = next->pending_result;
    next->pending_result = 0;
    return selected_result;
}

static int64_t plasma_fork_process(uint64_t clone_flags,
                                   uint64_t child_stack,
                                   uint64_t parent_tid_address,
                                   uint64_t child_tid_address,
                                   bool clone_call) {
    plasma_scheduler_init();
    struct plasma_process *parent = plasma_current_process();
    if (parent == 0) return -LINUX_EAGAIN;

    if (clone_call) {
        if ((clone_flags & ~PLASMA_CLONE_KNOWN) != 0) return -LINUX_ENOSYS;
        if ((clone_flags & PLASMA_CLONE_UNSUPPORTED) != 0) return -LINUX_ENOSYS;
        const uint64_t signal = clone_flags & 0xffull;
        if (signal != 0 && signal != PLASMA_SIGCHLD) return -LINUX_EINVAL;
    }

    const int free_slot = plasma_find_free_slot();
    if (free_slot < 0) return -LINUX_EAGAIN;

    if (clone_call && (clone_flags & CLONE_PARENT_SETTID) != 0 && parent_tid_address != 0)
        if (!user_store_u32(parent_tid_address, (uint32_t)plasma_next_pid)) return -LINUX_EFAULT;

    bytes_zero(&plasma_child_build_image, sizeof(plasma_child_build_image));
    if (!plasma_clone_image(&image, &plasma_child_build_image)) return -LINUX_ENOMEM;

    struct plasma_process *child = &plasma_processes[free_slot];
    bytes_zero(child, sizeof(*child));
    child->state = PLASMA_PROC_RUNNABLE;
    child->pid = plasma_next_pid++;
    child->ppid = parent->pid;
    child->pending_result = 0;
    child->fs_base = fs_base;
    child->foreground_pgrp = foreground_pgrp;
    bytes_copy(&child->image, &plasma_child_build_image, sizeof(child->image));
    bytes_zero(&plasma_child_build_image, sizeof(plasma_child_build_image));
    bytes_copy(child->files, rootfs_open_files, sizeof(rootfs_open_files));
    plasma_copy_c_string(child->cwd, sizeof(child->cwd), current_directory);
    plasma_copy_c_string(child->exec_path, sizeof(child->exec_path), plasma_exec_path);
    linux_process_capture_context(child->context);
    if (child_stack != 0) child->context[1] = child_stack;

    if (clone_call && (clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) != 0)
        child->clear_tid_address = child_tid_address;
    if (clone_call && (clone_flags & CLONE_CHILD_SETTID) != 0 && child_tid_address != 0) {
        struct shell_image saved_image;
        bytes_copy(&saved_image, &image, sizeof(image));
        bytes_copy(&image, &child->image, sizeof(image));
        const bool stored = user_store_u32(child_tid_address, (uint32_t)child->pid);
        bytes_copy(&image, &saved_image, sizeof(image));
        if (!stored) {
            plasma_destroy_detached_image(&child->image);
            bytes_zero(child, sizeof(*child));
            return -LINUX_EFAULT;
        }
    }

    serial_write("[linux:process] fork queued runnable child pid=");
    serial_u64((uint64_t)child->pid);
    serial_write("\n");
    return child->pid;
}

static int64_t plasma_process_exit(int status) {
    plasma_scheduler_init();
    struct plasma_process *process = plasma_current_process();
    if (process == 0 || process->pid == PLASMA_INIT_PID) {
        shell_exit_status = status & 0xff;
        shell_exit_seen = true;
        write_msr(IA32_FS_BASE_MSR, 0);
        fs_base = 0;
        return LINUX_EXIT_SENTINEL;
    }

    plasma_save_active(process);
    if (process->clear_tid_address != 0)
        (void)user_store_u32(process->clear_tid_address, 0);
    process->exit_status = status & 0xff;
    process->state = PLASMA_PROC_ZOMBIE;
    plasma_wake_waiters_for(process);
    serial_write("[linux:process] process exited pid=");
    serial_u64((uint64_t)process->pid);
    serial_write("\n");
    return 0;
}

static int64_t plasma_wait4(int requested_pid,
                            uint64_t status_address,
                            uint64_t options,
                            uint64_t rusage_address) {
    plasma_scheduler_init();
    struct plasma_process *parent = plasma_current_process();
    if (parent == 0) return -LINUX_ECHILD;

    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *child = &plasma_processes[i];
        if (child->state != PLASMA_PROC_ZOMBIE ||
            !plasma_wait_matches(requested_pid, child, parent->pid)) continue;
        if (status_address != 0) {
            const uint32_t wait_status = (uint32_t)(child->exit_status & 0xff) << 8;
            if (!user_store_u32(status_address, wait_status)) return -LINUX_EFAULT;
        }
        if (rusage_address != 0 && !user_zero(rusage_address, 144)) return -LINUX_EFAULT;
        const int pid = child->pid;
        if (child->image.space != vmm_current_space()) {
            plasma_destroy_detached_image(&child->image);
            bytes_zero(child, sizeof(*child));
        }
        return pid;
    }

    if (!plasma_has_matching_live_child(parent->pid, requested_pid)) return -LINUX_ECHILD;
    if ((options & PLASMA_WNOHANG) != 0) return 0;

    plasma_save_active(parent);
    parent->wait_target = requested_pid;
    parent->wait_status_address = status_address;
    parent->wait_rusage_address = rusage_address;
    parent->state = PLASMA_PROC_BLOCKED_WAIT;
    return 0;
}

'''
    text = replace_once(
        text,
        "static int64_t shell_dispatch(uint64_t number,\n",
        process_code + "static int64_t shell_dispatch(uint64_t number,\n",
    )

    text = replace_once(
        text,
        "    case SYS_EXECVE: return plasma_execve(a1, a2, a3);\n",
        "    case SYS_CLONE:\n"
        "        return plasma_fork_process(a1, a2, a3, a4, true);\n"
        "    case SYS_FORK:\n"
        "        return plasma_fork_process(PLASMA_SIGCHLD, 0, 0, 0, false);\n"
        "    case SYS_VFORK:\n"
        "        return plasma_fork_process(PLASMA_SIGCHLD | CLONE_VFORK, 0, 0, 0, false);\n"
        "    case SYS_EXECVE: return plasma_execve(a1, a2, a3);\n",
    )

    old_exit = '''    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        shell_exit_status = (int)(a1 & 0xffu);
        shell_exit_seen = true;
        write_msr(IA32_FS_BASE_MSR, 0);
        fs_base = 0;
        return LINUX_EXIT_SENTINEL;
'''
    new_exit = '''    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        return plasma_process_exit((int)(a1 & 0xffu));
'''
    text = replace_once(text, old_exit, new_exit)

    text = replace_once(
        text,
        "    case SYS_SET_TID_ADDRESS: return 1;\n",
        "    case SYS_SET_TID_ADDRESS: {\n"
        "        plasma_scheduler_init();\n"
        "        struct plasma_process *process = plasma_current_process();\n"
        "        if (process != 0) process->clear_tid_address = a1;\n"
        "        return process != 0 ? process->pid : PLASMA_INIT_PID;\n"
        "    }\n",
    )

    text = replace_once(
        text,
        "    case SYS_GETPID:\n    case SYS_GETTID: return 1;\n",
        "    case SYS_GETPID:\n"
        "    case SYS_GETTID: {\n"
        "        plasma_scheduler_init();\n"
        "        struct plasma_process *process = plasma_current_process();\n"
        "        return process != 0 ? process->pid : PLASMA_INIT_PID;\n"
        "    }\n",
    )
    text = replace_once(
        text,
        "    case SYS_GETPPID: return 0;\n",
        "    case SYS_GETPPID: {\n"
        "        plasma_scheduler_init();\n"
        "        struct plasma_process *process = plasma_current_process();\n"
        "        return process != 0 ? process->ppid : 0;\n"
        "    }\n",
    )
    text = replace_once(
        text,
        "    case SYS_WAIT4: return -LINUX_ECHILD;\n",
        "    case SYS_WAIT4: return plasma_wait4((int)a1, a2, a3, a4);\n",
    )

    abandon_old = '''    shell_exit_status = 126;
    shell_exit_seen = true;
    write_msr(IA32_FS_BASE_MSR, 0);
    fs_base = 0;
    return LINUX_EXIT_SENTINEL;
'''
    abandon_new = '''    if (plasma_scheduler_ready) {
        struct plasma_process *process = plasma_current_process();
        if (process != 0 && process->pid != PLASMA_INIT_PID)
            return plasma_process_exit(126);
    }
    shell_exit_status = 126;
    shell_exit_seen = true;
    write_msr(IA32_FS_BASE_MSR, 0);
    fs_base = 0;
    return LINUX_EXIT_SENTINEL;
'''
    text = replace_once(text, abandon_old, abandon_new)

    old_router = '''int64_t linux_busybox_syscall_router(uint64_t number,
                                     uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    if (shell_active) return shell_dispatch(number, a1, a2, a3, a4, a5, a6);
    return linux_busybox_syscall_dispatch(number, a1, a2, a3, a4, a5, a6);
}
'''
    new_router = '''int64_t linux_busybox_syscall_router(uint64_t number,
                                     uint64_t a1, uint64_t a2, uint64_t a3,
                                     uint64_t a4, uint64_t a5, uint64_t a6) {
    if (!shell_active)
        return linux_busybox_syscall_dispatch(number, a1, a2, a3, a4, a5, a6);
    plasma_cleanup_reaped();
    const int64_t result = shell_dispatch(number, a1, a2, a3, a4, a5, a6);
    return plasma_schedule_after_syscall(result);
}
'''
    text = replace_once(text, old_router, new_router)

    path.write_text(text, encoding="utf-8")
    print(f"Added cooperative multi-process scheduler + fork/clone/wait4: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
