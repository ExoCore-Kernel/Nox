#!/usr/bin/env python3
"""Remove bring-up-only scheduler overhead without changing userspace semantics.

The Plasma ABI originally embedded a 131072-entry shell_page array in every
process and copied the entire struct shell_image on each syscall-boundary context
switch.  At 24 bytes per page entry that is over 3 MiB copied in each direction,
even when a syscall did not modify the address space.  Threads also copied that
same metadata despite CLONE_VM, and runtime/rootfs descriptor tables were copied
on every switch despite CLONE_FILES.

This finalizer makes the current process/thread-group state authoritative in
place:

* the global shell ABI accesses the active process's shell_image through a
  pointer, so context switches change a pointer instead of copying the maximum
  page-capacity array;
* rootfs/runtime/low descriptor tables are active pointers too.  Threads point
  at the group leader's tables, giving CLONE_FILES real shared table semantics;
* thread slots are cleared without touching the multi-megabyte embedded image;
* image reset/copy helpers only touch live metadata and page entries;
* scheduler wakeups temporarily select the target address-space metadata by
  pointer instead of copying it through a 3 MiB scratch object;
* a small syscall quantum avoids a full scheduler save/restore after every
  syscall while still switching immediately when the current task blocks.

With the optional quiet mode (default in run-plasma-bringup.sh), high-frequency
bring-up traces are removed while keeping state/error/milestone diagnostics.
Set PLASMA_TRACE=1 to retain verbose I/O/Xorg tracing for debugging.
"""
from __future__ import annotations

import pathlib
import re
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected performance fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def optional_rep(text: str, old: str, new: str) -> str:
    return text.replace(old, new)


def main() -> int:
    if len(sys.argv) not in (2, 3):
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C [verbose:0|1]", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    verbose = len(sys.argv) == 3 and sys.argv[2] == "1"
    text = path.read_text(encoding="utf-8")

    # ------------------------------------------------------------------
    # Active address-space and descriptor state
    # ------------------------------------------------------------------
    text = rep(
        text,
        "static struct shell_image image;\n",
        "static struct shell_image plasma_boot_image;\n"
        "static struct shell_image *plasma_active_image = &plasma_boot_image;\n",
    )
    text = rep(
        text,
        "static struct rootfs_open_file rootfs_open_files[ROOTFS_FD_COUNT];\n",
        "static struct rootfs_open_file plasma_boot_rootfs_open_files[ROOTFS_FD_COUNT];\n"
        "static struct rootfs_open_file (*plasma_active_rootfs_open_files)[ROOTFS_FD_COUNT] =\n"
        "    &plasma_boot_rootfs_open_files;\n",
    )
    text = rep(
        text,
        "static struct plasma_runtime_fd plasma_runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n"
        "static struct plasma_runtime_fd plasma_low_runtime_fds[10];\n",
        "static struct plasma_runtime_fd plasma_boot_runtime_fds[PLASMA_RUNTIME_FD_COUNT];\n"
        "static struct plasma_runtime_fd plasma_boot_low_runtime_fds[10];\n"
        "static struct plasma_runtime_fd (*plasma_active_runtime_fds)[PLASMA_RUNTIME_FD_COUNT] =\n"
        "    &plasma_boot_runtime_fds;\n"
        "static struct plasma_runtime_fd (*plasma_active_low_runtime_fds)[10] =\n"
        "    &plasma_boot_low_runtime_fds;\n",
    )

    # Rewrite only the standalone global ABI objects.  process->image and the
    # per-process arrays intentionally keep their field names.
    text = re.sub(r"(?<![>.])\bimage\.", "plasma_active_image->", text)
    text = re.sub(r"&image\b", "plasma_active_image", text)
    text = text.replace("sizeof(image)", "sizeof(*plasma_active_image)")
    text = re.sub(r"\brootfs_open_files\b", "(*plasma_active_rootfs_open_files)", text)
    text = re.sub(r"\bplasma_runtime_fds\b", "(*plasma_active_runtime_fds)", text)
    text = re.sub(r"\bplasma_low_runtime_fds\b", "(*plasma_active_low_runtime_fds)", text)

    # Helpers live after struct plasma_process is complete and before its first
    # image-destruction routine.  Page-array bytes beyond page_count are never
    # semantically live, so resetting them is unnecessary.
    destroy_anchor = "static void plasma_destroy_detached_image(struct shell_image *victim) {\n"
    helpers = r'''static void plasma_reset_image_state(struct shell_image *target) {
    if (target == 0) return;
    target->space = VMM_INVALID_SPACE;
    target->entry = 0;
    target->stack_pointer = 0;
    target->phdr = 0;
    target->phnum = 0;
    target->phentsize = 0;
    target->brk_base = 0;
    target->brk_current = 0;
    target->mmap_next = SHELL_MMAP_BASE;
    target->page_count = 0;
}

static void plasma_copy_image_state(struct shell_image *destination,
                                    const struct shell_image *source) {
    if (destination == 0 || source == 0 || destination == source) return;
    destination->space = source->space;
    destination->entry = source->entry;
    destination->stack_pointer = source->stack_pointer;
    destination->phdr = source->phdr;
    destination->phnum = source->phnum;
    destination->phentsize = source->phentsize;
    destination->brk_base = source->brk_base;
    destination->brk_current = source->brk_current;
    destination->mmap_next = source->mmap_next;
    destination->page_count = source->page_count;
    if (source->page_count != 0)
        bytes_copy(destination->pages, source->pages,
                   source->page_count * sizeof(struct shell_page));
}

static void plasma_clear_process_slot_except_image(struct plasma_process *process) {
    if (process == 0) return;
    /* image.pages[] is by far the largest field.  Its stale entries are not
     * observable once page_count is zero, so clear the small prefix/tail only. */
    bytes_zero(process, offsetof(struct plasma_process, image));
    const size_t tail = offsetof(struct plasma_process, files);
    bytes_zero((uint8_t *)process + tail, sizeof(*process) - tail);
    plasma_reset_image_state(&process->image);
}

'''
    text = rep(text, destroy_anchor, helpers + destroy_anchor)

    # Destroyed images no longer need a multi-megabyte memset.
    text = rep(
        text,
        "    bytes_zero(victim, sizeof(*victim));\n",
        "    plasma_reset_image_state(victim);\n",
    )

    # fork() still performs a real eager private address-space copy.  Avoid
    # copying the maximum-capacity metadata array before the page-by-page clone;
    # only the scalar image metadata is inherited here.
    clone_header_old = (
        "    bytes_copy(child, parent, sizeof(*child));\n"
        "    child->space = vmm_create_address_space();\n"
        "    child->page_count = 0;\n"
    )
    clone_header_new = (
        "    plasma_reset_image_state(child);\n"
        "    child->entry = parent->entry;\n"
        "    child->stack_pointer = parent->stack_pointer;\n"
        "    child->phdr = parent->phdr;\n"
        "    child->phnum = parent->phnum;\n"
        "    child->phentsize = parent->phentsize;\n"
        "    child->brk_base = parent->brk_base;\n"
        "    child->brk_current = parent->brk_current;\n"
        "    child->mmap_next = parent->mmap_next;\n"
        "    child->space = vmm_create_address_space();\n"
    )
    text = rep(text, clone_header_old, clone_header_new)

    # Scheduler slots are static BSS and therefore already zero before the only
    # scheduler initialization.  Do not clear 24 embedded multi-megabyte images.
    text = rep(text, "    bytes_zero(plasma_processes, sizeof(plasma_processes));\n", "")

    # Save/restore no longer copies address-space or descriptor metadata.  Those
    # objects are already canonical in their owning process slot.
    text = rep(
        text,
        "    bytes_copy(&process->image, plasma_active_image, sizeof(*plasma_active_image));\n",
        "",
    )
    text = rep(
        text,
        "    bytes_copy(process->files, (*plasma_active_rootfs_open_files), sizeof((*plasma_active_rootfs_open_files)));\n",
        "",
    )
    text = rep(
        text,
        "    bytes_copy(process->runtime_fds, (*plasma_active_runtime_fds), sizeof((*plasma_active_runtime_fds)));\n",
        "",
    )
    text = rep(
        text,
        "    bytes_copy(process->low_runtime_fds, (*plasma_active_low_runtime_fds), sizeof((*plasma_active_low_runtime_fds)));\n",
        "",
    )

    text = rep(
        text,
        "    bytes_copy(plasma_active_image, &process->image, sizeof(*plasma_active_image));\n",
        "    struct plasma_process *plasma_state_owner = plasma_thread_group_leader(process);\n"
        "    if (plasma_state_owner == 0) plasma_state_owner = process;\n"
        "    plasma_active_image = &plasma_state_owner->image;\n"
        "    plasma_active_rootfs_open_files = &plasma_state_owner->files;\n"
        "    plasma_active_runtime_fds = &plasma_state_owner->runtime_fds;\n"
        "    plasma_active_low_runtime_fds = &plasma_state_owner->low_runtime_fds;\n",
    )
    text = rep(
        text,
        "    bytes_copy((*plasma_active_rootfs_open_files), process->files, sizeof((*plasma_active_rootfs_open_files)));\n",
        "",
    )
    text = rep(
        text,
        "    bytes_copy((*plasma_active_runtime_fds), process->runtime_fds, sizeof((*plasma_active_runtime_fds)));\n",
        "",
    )
    text = rep(
        text,
        "    bytes_copy((*plasma_active_low_runtime_fds), process->low_runtime_fds, sizeof((*plasma_active_low_runtime_fds)));\n",
        "",
    )

    # Hand the pre-scheduler boot state to pid 1 once, then make its in-place
    # structures authoritative.  This is the only normal full live-image copy.
    init_anchor = (
        "    plasma_current_slot = 0;\n"
        "    plasma_scheduler_ready = true;\n"
    )
    init_new = (
        "    plasma_current_slot = 0;\n"
        "    plasma_copy_image_state(&init->image, plasma_active_image);\n"
        "    bytes_copy(init->files, (*plasma_active_rootfs_open_files), sizeof(init->files));\n"
        "    bytes_copy(init->runtime_fds, (*plasma_active_runtime_fds), sizeof(init->runtime_fds));\n"
        "    bytes_copy(init->low_runtime_fds, (*plasma_active_low_runtime_fds), sizeof(init->low_runtime_fds));\n"
        "    plasma_active_image = &init->image;\n"
        "    plasma_active_rootfs_open_files = &init->files;\n"
        "    plasma_active_runtime_fds = &init->runtime_fds;\n"
        "    plasma_active_low_runtime_fds = &init->low_runtime_fds;\n"
        "    plasma_scheduler_ready = true;\n"
    )
    text = rep(text, init_anchor, init_new)

    # CLONE_VM/CLONE_FILES threads use the leader's active pointers.  Their own
    # embedded image and descriptor arrays no longer need scheduler-boundary
    # resynchronization.
    for fragment in (
        "    bytes_copy(&process->image, &leader->image, sizeof(process->image));\n",
        "    bytes_copy(process->files, leader->files, sizeof(process->files));\n",
        "    bytes_copy(process->runtime_fds, leader->runtime_fds, sizeof(process->runtime_fds));\n",
        "    bytes_copy(process->low_runtime_fds, leader->low_runtime_fds, sizeof(process->low_runtime_fds));\n",
        "    bytes_copy(&leader->image, &process->image, sizeof(leader->image));\n",
        "    bytes_copy(leader->files, process->files, sizeof(leader->files));\n",
        "    bytes_copy(leader->runtime_fds, process->runtime_fds, sizeof(leader->runtime_fds));\n",
        "    bytes_copy(leader->low_runtime_fds, process->low_runtime_fds, sizeof(leader->low_runtime_fds));\n",
    ):
        text = rep(text, fragment, "")

    # Clear/reuse process slots without touching the dormant page array.
    child_clear_count = text.count("    bytes_zero(child, sizeof(*child));\n")
    if child_clear_count == 0:
        raise RuntimeError("no child scheduler-slot clears found")
    text = text.replace(
        "    bytes_zero(child, sizeof(*child));\n",
        "    plasma_clear_process_slot_except_image(child);\n",
    )
    process_clear_count = text.count("        bytes_zero(process, sizeof(*process));\n")
    if process_clear_count:
        text = text.replace(
            "        bytes_zero(process, sizeof(*process));\n",
            "        plasma_clear_process_slot_except_image(process);\n",
        )

    # Thread clone: only retain the shared space identity in the thread slot;
    # plasma_load_active() points execution at the group leader's real image.
    text = rep(
        text,
        "    bytes_copy(&child->image, plasma_active_image, sizeof(child->image));\n",
        "    child->image.space = plasma_active_image->space;\n"
        "    child->image.page_count = 0;\n",
    )

    # The private fork staging image is still useful for failure atomicity, but
    # resetting/copying it need only touch live metadata rather than 3 MiB.
    text = text.replace(
        "    bytes_zero(&plasma_child_build_image, sizeof(plasma_child_build_image));\n",
        "    plasma_reset_image_state(&plasma_child_build_image);\n",
    )
    text = rep(
        text,
        "    bytes_copy(&child->image, &plasma_child_build_image, sizeof(child->image));\n",
        "    plasma_copy_image_state(&child->image, &plasma_child_build_image);\n",
    )

    # ------------------------------------------------------------------
    # Temporary waiter address-space selection: pointer swap, not 3 MiB copy.
    # ------------------------------------------------------------------
    scratch_pair = re.compile(
        r"(?P<indent>^[ \t]*)bytes_copy\(&plasma_scheduler_scratch_image, plasma_active_image, sizeof\(\*plasma_active_image\)\);\n"
        r"(?P=indent)bytes_copy\(plasma_active_image, &(?P<var>[A-Za-z_][A-Za-z0-9_]*)->image, sizeof\(\*plasma_active_image\)\);",
        re.MULTILINE,
    )

    def scratch_repl(match: re.Match[str]) -> str:
        indent = match.group("indent")
        var = match.group("var")
        return (
            f"{indent}struct shell_image *plasma_saved_active_image = plasma_active_image;\n"
            f"{indent}struct plasma_process *plasma_temporary_mm_owner = plasma_thread_group_leader({var});\n"
            f"{indent}if (plasma_temporary_mm_owner == 0) plasma_temporary_mm_owner = {var};\n"
            f"{indent}plasma_active_image = &plasma_temporary_mm_owner->image;"
        )

    text, scratch_pairs = scratch_pair.subn(scratch_repl, text)
    restores = text.count(
        "bytes_copy(plasma_active_image, &plasma_scheduler_scratch_image, sizeof(*plasma_active_image));"
    )
    if scratch_pairs == 0 or restores != scratch_pairs:
        raise RuntimeError(
            f"scheduler scratch pairing mismatch: starts={scratch_pairs} restores={restores}"
        )
    text = text.replace(
        "bytes_copy(plasma_active_image, &plasma_scheduler_scratch_image, sizeof(*plasma_active_image));",
        "plasma_active_image = plasma_saved_active_image;",
    )
    # The scratch image is now unused.
    text = optional_rep(text, "static struct shell_image plasma_scheduler_scratch_image;\n", "")

    # ------------------------------------------------------------------
    # A real cooperative scheduling quantum.
    # ------------------------------------------------------------------
    text = rep(
        text,
        "static bool plasma_scheduler_ready;\n",
        "static bool plasma_scheduler_ready;\n"
        "#define PLASMA_SYSCALL_QUANTUM 32u\n"
        "static uint32_t plasma_scheduler_quantum_remaining = PLASMA_SYSCALL_QUANTUM;\n",
    )

    schedule_anchor = (
        "    if (result == LINUX_EXIT_SENTINEL) return result;\n\n"
        "    if (current->state == PLASMA_PROC_RUNNING) {\n"
    )
    schedule_fast = (
        "    if (result == LINUX_EXIT_SENTINEL) return result;\n\n"
        "    /* Keep executing the current task for a short syscall quantum.\n"
        "     * Blocking syscalls change state before reaching here and therefore\n"
        "     * still switch immediately.  If nobody else is runnable there is\n"
        "     * no reason to save/restore scheduler state at all. */\n"
        "    if (current->state == PLASMA_PROC_RUNNING) {\n"
        "        bool other_runnable = false;\n"
        "        for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {\n"
        "            if (i != plasma_current_slot &&\n"
        "                plasma_processes[i].state == PLASMA_PROC_RUNNABLE) {\n"
        "                other_runnable = true;\n"
        "                break;\n"
        "            }\n"
        "        }\n"
        "        if (!other_runnable) return result;\n"
        "        if (plasma_scheduler_quantum_remaining > 1u) {\n"
        "            --plasma_scheduler_quantum_remaining;\n"
        "            return result;\n"
        "        }\n"
        "    }\n\n"
        "    if (current->state == PLASMA_PROC_RUNNING) {\n"
    )
    text = rep(text, schedule_anchor, schedule_fast)
    text = rep(
        text,
        "    plasma_current_slot = next_slot;\n"
        "    struct plasma_process *next = &plasma_processes[next_slot];\n",
        "    plasma_current_slot = next_slot;\n"
        "    plasma_scheduler_quantum_remaining = PLASMA_SYSCALL_QUANTUM;\n"
        "    struct plasma_process *next = &plasma_processes[next_slot];\n",
    )
    text = rep(
        text,
        "    serial_write(\"[linux:process] cooperative scheduler online; init pid=1\\n\");\n",
        "    plasma_scheduler_quantum_remaining = PLASMA_SYSCALL_QUANTUM;\n"
        "    serial_write(\"[linux:perf] zero-copy scheduler state + syscall quantum enabled\\n\");\n"
        "    serial_write(\"[linux:process] cooperative scheduler online; init pid=1\\n\");\n",
    )

    # Unsupported-syscall spam is expensive over emulated serial. Keep the first
    # occurrence of each syscall number, which preserves the diagnostic signal.
    unknown_pattern = re.compile(
        r"static void log_unknown\(uint64_t number\) \{.*?^\}\n",
        re.MULTILINE | re.DOTALL,
    )
    unknown_new = r'''static bool plasma_unknown_syscall_seen[512];
static void log_unknown(uint64_t number) {
    if (number < 512u) {
        if (plasma_unknown_syscall_seen[number]) return;
        plasma_unknown_syscall_seen[number] = true;
    }
    serial_write("[linux:bash] unsupported syscall ");
    serial_u64(number);
    serial_write(" -> -ENOSYS\n");
}
'''
    text, unknown_count = unknown_pattern.subn(unknown_new, text, count=1)
    if unknown_count != 1:
        raise RuntimeError(f"expected one log_unknown function, found {unknown_count}")

    # ------------------------------------------------------------------
    # Quiet normal bring-up; PLASMA_TRACE=1 retains detailed diagnostics.
    # ------------------------------------------------------------------
    if not verbose:
        # Qt's plugin debugger is useful only while diagnosing discovery/load
        # failures and produces a large amount of guest serial output.
        text = text.replace("export QT_DEBUG_PLUGINS=1; ", "")

        text = optional_rep(
            text,
            '''    if (entry->type == PLASMA_RT_SOCKET) {
        serial_write("[linux:socket] tx attempt fd=");
        serial_u64((uint64_t)fd);
        serial_write(" object=");
        serial_u64((uint64_t)entry->object);
        serial_write(" len=");
        serial_u64(length);
        serial_write("\n");
    }
''',
            "",
        )
        text = optional_rep(
            text,
            '''    if (!user_range(address, length, false)) {
        if (entry->type == PLASMA_RT_SOCKET)
            serial_write("[linux:socket] tx failed: user buffer EFAULT\n");
        return -LINUX_EFAULT;
    }
''',
            '''    if (!user_range(address, length, false)) return -LINUX_EFAULT;
''',
        )
        text = optional_rep(
            text,
            '''        serial_write("[linux:socket] tx queued peer=");
        serial_u64((uint64_t)(sock->peer < 0 ? 0 : sock->peer));
        serial_write(" bytes=");
        serial_u64(done);
        serial_write("\n");
''',
            "",
        )
        text = optional_rep(
            text,
            '''        if (runtime->type == PLASMA_RT_SOCKET) {
            serial_write("[linux:socket] fcntl fd=");
            serial_u64((uint64_t)fd);
            serial_write(" cmd=");
            serial_u64(command);
            serial_write(" arg=");
            serial_u64(argument);
            serial_write("\n");
        }

''',
            "",
        )
        text = optional_rep(
            text,
            '        serial_write("[linux:x11] setitimer accepted as inert X11 timeout\\n");\n',
            "",
        )
        text = optional_rep(
            text,
            '        serial_write("[linux:fbdev] FBIOPUTCMAP accepted as truecolor no-op\\n");\n',
            "",
        )

        # X11 poll and futex wait/wake traces occur in hot loops.  Milestone
        # connection/thread/process logs remain enabled.
        hot_blocks = [
            '''    serial_write("[linux:socket] blocking poll pid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" object=");
    serial_u64((uint64_t)entry->object);
    serial_write(" timeout=");
    serial_u64(timeout < 0 ? 0ull : (uint64_t)timeout);
    serial_write("\n");
''',
            '''        serial_write("[linux:socket] woke poll pid=");
        serial_u64((uint64_t)poller->pid);
        serial_write(" object=");
        serial_u64((uint64_t)object);
        serial_write(" revents=POLLIN\n");
''',
            '''        serial_write("[linux:futex] wake address=");
        serial_u64(address);
        serial_write(" count=");
        serial_u64(woke);
        serial_write("\n");
''',
            '''    serial_write("[linux:futex] wait tid=");
    serial_u64((uint64_t)current->pid);
    serial_write(" address=");
    serial_u64(address);
    serial_write(" expected=");
    serial_u64((uint64_t)value);
    serial_write("\n");
''',
        ]
        for block in hot_blocks:
            text = optional_rep(text, block, "")

    path.write_text(text, encoding="utf-8")
    mode = "verbose" if verbose else "quiet"
    print(
        f"Finalized Plasma performance: zero-copy active state, syscall quantum, {mode} tracing: {path}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
