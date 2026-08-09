#!/usr/bin/env python3
"""Add the first fork/clone/wait process model to the Plasma bring-up ABI.

The first milestone is deliberately serialized: the parent is suspended while a
copied child address space runs.  When the child exits, Twilight restores the
parent at the instruction following fork()/clone(), with the child already a
zombie for wait4() to reap.  This gives real fork -> execve -> exit -> wait
semantics for ordinary shell commands without pretending that concurrent
scheduling or CLONE_VM threads exist yet.
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
    process_decl = exec_decl + (
        "extern void linux_process_capture_context(uint64_t *words);\n"
        "extern void linux_process_restore_context(const uint64_t *words);\n"
        "extern void linux_process_set_return_space(uint64_t space);\n"
    )
    text = replace_once(text, exec_decl, process_decl)

    open_files_anchor = "static struct rootfs_open_file rootfs_open_files[ROOTFS_FD_COUNT];\n"
    process_state = open_files_anchor + r'''

#define PLASMA_PROCESS_CONTEXT_WORDS 15u
#define PLASMA_PARENT_PID 1
#define PLASMA_CHILD_PID  2
#define PLASMA_SIGCHLD    17ull

#define CLONE_PARENT_SETTID 0x00100000ull
#define CLONE_CHILD_CLEARTID 0x00200000ull
#define CLONE_CHILD_SETTID  0x01000000ull
#define PLASMA_CLONE_PROCESS_FLAGS \
    (0xffull | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID)

static uint64_t plasma_parent_context[PLASMA_PROCESS_CONTEXT_WORDS];
static struct shell_image plasma_parent_image;
static struct shell_image plasma_child_build_image;
static struct shell_image plasma_zombie_image;
static struct rootfs_open_file plasma_parent_open_files[ROOTFS_FD_COUNT];
static char plasma_parent_cwd[64];
static char plasma_parent_exec_path[PLASMA_EXEC_STRING];
static uint64_t plasma_parent_fs_base;
static int plasma_parent_foreground_pgrp;
static int plasma_current_pid = PLASMA_PARENT_PID;
static bool plasma_parent_suspended;
static bool plasma_zombie_valid;
static int plasma_zombie_pid;
static int plasma_zombie_status;
static uint64_t plasma_child_tid_address;

static int64_t plasma_child_exit(int status);
'''
    text = replace_once(text, open_files_anchor, process_state)

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

static void plasma_destroy_detached_image(struct shell_image *victim) {
    if (victim == 0 || victim->space == VMM_INVALID_SPACE) return;
    if (victim->space == vmm_current_space()) return;

    for (size_t i = 0; i < victim->page_count; ++i) {
        uint64_t physical = 0;
        if (vmm_unmap_page(victim->space, victim->pages[i].va, &physical) && physical != 0)
            (void)pmm_free_page(physical);
    }
    (void)vmm_destroy_address_space(victim->space);
    *victim = (struct shell_image){0};
}

static bool plasma_clone_image(const struct shell_image *parent,
                               struct shell_image *child) {
    if (parent == 0 || child == 0 || parent->space == VMM_INVALID_SPACE)
        return false;

    *child = *parent;
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
        child->pages[child->page_count++] = (struct shell_page){
            .va = source_page->va,
            .phys = physical,
            .flags = source_page->flags,
        };
    }
    return true;

fail:
    plasma_destroy_detached_image(child);
    return false;
}

static void plasma_save_parent_process(void) {
    linux_process_capture_context(plasma_parent_context);
    plasma_parent_image = image;
    bytes_copy(plasma_parent_open_files, rootfs_open_files,
               sizeof(plasma_parent_open_files));
    plasma_copy_c_string(plasma_parent_cwd, sizeof(plasma_parent_cwd), current_directory);
    plasma_copy_c_string(plasma_parent_exec_path, sizeof(plasma_parent_exec_path),
                         plasma_exec_path);
    plasma_parent_fs_base = fs_base;
    plasma_parent_foreground_pgrp = foreground_pgrp;
}

static void plasma_restore_parent_process(void) {
    image = plasma_parent_image;
    plasma_parent_image = (struct shell_image){0};
    bytes_copy(rootfs_open_files, plasma_parent_open_files,
               sizeof(rootfs_open_files));
    plasma_copy_c_string(current_directory, sizeof(current_directory), plasma_parent_cwd);
    plasma_copy_c_string(plasma_exec_path, sizeof(plasma_exec_path),
                         plasma_parent_exec_path);
    fs_base = plasma_parent_fs_base;
    foreground_pgrp = plasma_parent_foreground_pgrp;
    write_msr(IA32_FS_BASE_MSR, fs_base);
    linux_process_restore_context(plasma_parent_context);
    linux_process_set_return_space(image.space);
    plasma_current_pid = PLASMA_PARENT_PID;
    plasma_parent_suspended = false;
}

static int64_t plasma_fork_process(uint64_t clone_flags,
                                   uint64_t child_stack,
                                   uint64_t parent_tid_address,
                                   uint64_t child_tid_address,
                                   bool clone_call) {
    if (plasma_current_pid != PLASMA_PARENT_PID || plasma_parent_suspended ||
        plasma_zombie_valid)
        return -LINUX_EAGAIN;

    if (clone_call) {
        if ((clone_flags & ~PLASMA_CLONE_PROCESS_FLAGS) != 0)
            return -LINUX_ENOSYS;
        const uint64_t signal = clone_flags & 0xffull;
        if (signal != 0 && signal != PLASMA_SIGCHLD) return -LINUX_EINVAL;
    }

    if (clone_call && (clone_flags & CLONE_PARENT_SETTID) != 0 &&
        parent_tid_address != 0) {
        if (!user_store_u32(parent_tid_address, PLASMA_CHILD_PID))
            return -LINUX_EFAULT;
    }

    plasma_child_build_image = (struct shell_image){0};
    if (!plasma_clone_image(&image, &plasma_child_build_image)) return -LINUX_ENOMEM;

    plasma_save_parent_process();
    image = plasma_child_build_image;
    plasma_child_build_image = (struct shell_image){0};
    plasma_parent_suspended = true;
    plasma_current_pid = PLASMA_CHILD_PID;
    plasma_child_tid_address =
        clone_call && (clone_flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) != 0
            ? child_tid_address : 0;

    uint64_t child_context[PLASMA_PROCESS_CONTEXT_WORDS];
    linux_process_capture_context(child_context);
    if (child_stack != 0) child_context[1] = child_stack;
    linux_process_restore_context(child_context);

    if (clone_call && (clone_flags & CLONE_CHILD_SETTID) != 0 &&
        child_tid_address != 0) {
        if (!user_store_u32(child_tid_address, PLASMA_CHILD_PID)) {
            plasma_zombie_image = image;
            plasma_zombie_valid = true;
            plasma_zombie_pid = PLASMA_CHILD_PID;
            plasma_zombie_status = 127;
            plasma_restore_parent_process();
            return -LINUX_EFAULT;
        }
    }

    linux_process_set_return_space(image.space);
    serial_write("[linux:process] fork created serialized child pid=2\n");
    return 0;
}

static int64_t plasma_child_exit(int status) {
    if (plasma_current_pid == PLASMA_PARENT_PID || !plasma_parent_suspended)
        return LINUX_EXIT_SENTINEL;

    if (plasma_child_tid_address != 0) {
        (void)user_store_u32(plasma_child_tid_address, 0);
        plasma_child_tid_address = 0;
    }

    plasma_zombie_image = image;
    plasma_zombie_valid = true;
    plasma_zombie_pid = plasma_current_pid;
    plasma_zombie_status = status & 0xff;

    plasma_restore_parent_process();

    serial_write("[linux:process] child pid=2 exited; resuming parent pid=1\n");
    /* This return value becomes the parent's original fork()/clone() result. */
    return PLASMA_CHILD_PID;
}

static int64_t plasma_wait4(int64_t requested_pid,
                            uint64_t status_address,
                            uint64_t options,
                            uint64_t rusage_address) {
    (void)options;
    if (plasma_current_pid != PLASMA_PARENT_PID) return -LINUX_ECHILD;
    if (!plasma_zombie_valid) return -LINUX_ECHILD;
    if (requested_pid > 0 && requested_pid != plasma_zombie_pid) return -LINUX_ECHILD;

    if (status_address != 0) {
        const uint32_t wait_status = (uint32_t)(plasma_zombie_status & 0xff) << 8;
        if (!user_store_u32(status_address, wait_status)) return -LINUX_EFAULT;
    }
    if (rusage_address != 0 && !user_zero(rusage_address, 144)) return -LINUX_EFAULT;

    const int pid = plasma_zombie_pid;
    plasma_destroy_detached_image(&plasma_zombie_image);
    plasma_zombie_valid = false;
    plasma_zombie_pid = 0;
    plasma_zombie_status = 0;

    serial_write("[linux:process] wait4 reaped child pid=2\n");
    return pid;
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
        "        return plasma_fork_process(PLASMA_SIGCHLD, 0, 0, 0, false);\n"
        "    case SYS_EXECVE: return plasma_execve(a1, a2, a3);\n",
    )

    text = replace_once(
        text,
        "    case SYS_EXIT_GROUP:\n",
        "    case SYS_EXIT_GROUP:\n"
        "        if (plasma_current_pid != PLASMA_PARENT_PID)\n"
        "            return plasma_child_exit((int)(a1 & 0xffu));\n",
    )

    text = replace_once(
        text,
        "    case SYS_SET_TID_ADDRESS: return 1;\n",
        "    case SYS_SET_TID_ADDRESS:\n"
        "        plasma_child_tid_address = plasma_current_pid == PLASMA_CHILD_PID ? a1 : 0;\n"
        "        return plasma_current_pid;\n",
    )
    text = replace_once(
        text,
        "    case SYS_GETTID: return 1;\n",
        "    case SYS_GETTID: return plasma_current_pid;\n",
    )
    text = replace_once(
        text,
        "    case SYS_GETPID:\n",
        "    case SYS_GETPID: return plasma_current_pid;\n",
    )
    text = replace_once(
        text,
        "    case SYS_GETPPID: return 0;\n",
        "    case SYS_GETPPID: return plasma_current_pid == PLASMA_CHILD_PID ? PLASMA_PARENT_PID : 0;\n",
    )
    text = replace_once(
        text,
        "    case SYS_WAIT4: return -LINUX_ECHILD;\n",
        "    case SYS_WAIT4: return plasma_wait4((int64_t)a1, a2, a3, a4);\n",
    )

    # execve teardown failure inside a child must restore the suspended parent,
    # not tear down the entire interactive Linux session.
    text = replace_once(
        text,
        "    shell_exit_status = 126;\n    shell_exit_seen = true;\n",
        "    if (plasma_current_pid != PLASMA_PARENT_PID) return plasma_child_exit(126);\n"
        "    shell_exit_status = 126;\n    shell_exit_seen = true;\n",
    )

    path.write_text(text, encoding="utf-8")
    print(f"Added serialized fork/clone/vfork + wait4 process support: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
