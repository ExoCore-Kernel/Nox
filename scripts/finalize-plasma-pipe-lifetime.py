#!/usr/bin/env python3
"""Fix real pipe object lifetime in the Plasma runtime.

The original bring-up pipe2 implementation allocated from a 16-object static
pool but close(2) only cleared the descriptor slot.  A pipe object was therefore
never reusable, and partial pipe2 failures leaked both descriptors and objects.
This becomes visible when GLib/Qt creates many wakeup/process pipes.

This finalizer keeps the current finite-table kernel design but gives it proper
lifetime semantics for pipes: inherited/duplicated descriptors keep an object
alive, the last endpoint frees it, last-writer close still produces EOF, writes
without a reader return EPIPE, pipe2 is transactional, and pipe CLOEXEC ends are
closed after a successful execve.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    found = text.count(old)
    if found < count:
        raise RuntimeError(f"expected pipe-lifetime fragment not found ({found}): {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    # A finite kernel table is fine; 16 concurrent pipes was an unrealistically
    # tiny bring-up limit. Exhaustion is still reported, never faked.
    text = rep(text, "#define PLASMA_RUNTIME_FD_COUNT 48\n",
               "#define PLASMA_RUNTIME_FD_COUNT 128\n")
    text = rep(text, "#define PLASMA_PIPE_OBJECTS 16\n",
               "#define PLASMA_PIPE_OBJECTS 64\n")
    if "#define LINUX_ENFILE" not in text:
        text = rep(text,
                   "#define LINUX_ENOTDIR    20\n",
                   "#define LINUX_ENOTDIR    20\n#define LINUX_ENFILE      23\n#define LINUX_EMFILE      24\n")

    # A full per-process runtime fd table is EMFILE, not the old EBUSY.
    text = rep(text,
               "    return -LINUX_EBUSY;\n}\n\nstatic bool plasma_runtime_path",
               "    return -LINUX_EMFILE;\n}\n\nstatic bool plasma_runtime_path")

    # Declarations needed by runtime_write/close/execve, whose definitions occur
    # before the scheduler/process table where the liveness scans are defined.
    text = rep(text,
               "static bool plasma_pipe_has_live_writer(uint16_t object);\n"
               "static void plasma_runtime_notify_pipe_eof(uint16_t object);\n",
               "static bool plasma_pipe_has_live_writer(uint16_t object);\n"
               "static bool plasma_pipe_has_live_reader(uint16_t object);\n"
               "static void plasma_runtime_notify_pipe_eof(uint16_t object);\n")
    text = rep(text,
               "static bool plasma_runtime_initialized;\n",
               "static bool plasma_runtime_initialized;\n"
               "static void plasma_close_cloexec_pipe_fds(void);\n")
    text = rep(text,
               "static int64_t plasma_close_runtime(int fd) {\n",
               "static bool plasma_pipe_has_live_endpoint(uint16_t object);\n"
               "static void plasma_reclaim_pipe_if_unused(uint16_t object);\n\n"
               "static int64_t plasma_close_runtime(int fd) {\n")

    # Writing to a pipe after every read endpoint is gone is EPIPE.
    text = rep(text,
               "    if (entry->type == PLASMA_RT_PIPE_W) {\n"
               "        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];\n",
               "    if (entry->type == PLASMA_RT_PIPE_W) {\n"
               "        if (!plasma_pipe_has_live_reader(entry->object)) return -LINUX_EPIPE;\n"
               "        struct plasma_pipe_object *pipe = &plasma_pipes[entry->object];\n")

    # Preserve the proven last-writer EOF wake, then reclaim on the final read or
    # write descriptor close. A blocked read itself counts as a live endpoint.
    close_old = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const bool was_pipe_writer = entry->type == PLASMA_RT_PIPE_W &&
                                 entry->object < PLASMA_PIPE_OBJECTS;
    const uint16_t pipe_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    return 0;
}
'''
    close_new = r'''static int64_t plasma_close_runtime(int fd) {
    struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
    if (entry == 0) return -LINUX_EBADF;

    const bool was_pipe = (entry->type == PLASMA_RT_PIPE_R ||
                           entry->type == PLASMA_RT_PIPE_W) &&
                          entry->object < PLASMA_PIPE_OBJECTS;
    const bool was_pipe_writer = entry->type == PLASMA_RT_PIPE_W &&
                                 entry->object < PLASMA_PIPE_OBJECTS;
    const uint16_t pipe_object = entry->object;
    bytes_zero(entry, sizeof(*entry));

    if (was_pipe_writer && !plasma_pipe_has_live_writer(pipe_object))
        plasma_runtime_notify_pipe_eof(pipe_object);
    if (was_pipe) plasma_reclaim_pipe_if_unused(pipe_object);
    return 0;
}
'''
    text = rep(text, close_old, close_new)

    # pipe2 is atomic: either both fds are returned, or every allocation is
    # rolled back. Linux pipe2 currently accepts the flags we actually model.
    pipe2_old = r'''static int64_t plasma_pipe2(uint64_t pair_address, uint32_t flags) {
    plasma_runtime_init();
    int object = plasma_alloc_pipe_object();
    if (object < 0) return -LINUX_ENOMEM;
    int read_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_R, (uint16_t)object, flags);
    int write_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_W, (uint16_t)object, flags);
    if (read_fd < 0 || write_fd < 0) return -LINUX_EBUSY;
    int32_t pair[2] = { read_fd, write_fd };
    return user_copy_out(pair_address, pair, sizeof(pair)) ? 0 : -LINUX_EFAULT;
}
'''
    pipe2_new = r'''static int64_t plasma_pipe2(uint64_t pair_address, uint32_t flags) {
    plasma_runtime_init();
    const uint32_t allowed = PLASMA_O_NONBLOCK | PLASMA_SOCK_CLOEXEC;
    if ((flags & ~allowed) != 0) return -LINUX_EINVAL;

    const int object = plasma_alloc_pipe_object();
    if (object < 0) return -LINUX_ENFILE;

    const int read_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_R, (uint16_t)object, flags);
    if (read_fd < 0) {
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return read_fd;
    }
    const int write_fd = plasma_alloc_runtime_fd(PLASMA_RT_PIPE_W, (uint16_t)object, flags);
    if (write_fd < 0) {
        struct plasma_runtime_fd *read_entry = plasma_runtime_fd(read_fd);
        if (read_entry != 0) bytes_zero(read_entry, sizeof(*read_entry));
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return write_fd;
    }

    const int32_t pair[2] = { read_fd, write_fd };
    if (!user_copy_out(pair_address, pair, sizeof(pair))) {
        struct plasma_runtime_fd *read_entry = plasma_runtime_fd(read_fd);
        struct plasma_runtime_fd *write_entry = plasma_runtime_fd(write_fd);
        if (read_entry != 0) bytes_zero(read_entry, sizeof(*read_entry));
        if (write_entry != 0) bytes_zero(write_entry, sizeof(*write_entry));
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
        return -LINUX_EFAULT;
    }
    return 0;
}
'''
    text = rep(text, pipe2_old, pipe2_new)

    # These scans are defined after the pthread layer, so non-current CLONE_FILES
    # threads resolve through their canonical thread-group leader. The currently
    # running task uses the active descriptor tables because its saved snapshot
    # is not guaranteed current until the scheduler boundary.
    exit_anchor = "static int64_t plasma_process_exit(int status) {\n"
    helpers = r'''static bool plasma_fd_table_has_pipe_endpoint(
        const struct plasma_runtime_fd *table, size_t count,
        uint16_t object, bool readers_only) {
    if (table == 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (!table[i].used || table[i].object != object) continue;
        if (table[i].type == PLASMA_RT_PIPE_R) return true;
        if (!readers_only && table[i].type == PLASMA_RT_PIPE_W) return true;
    }
    return false;
}

static bool plasma_pipe_scan_live(uint16_t object, bool readers_only) {
    if (object >= PLASMA_PIPE_OBJECTS || !plasma_pipes[object].used) return false;
    struct plasma_process *current = plasma_current_process();
    for (size_t i = 0; i < PLASMA_MAX_PROCESSES; ++i) {
        struct plasma_process *process = &plasma_processes[i];
        if (process->state == PLASMA_PROC_FREE || process->state == PLASMA_PROC_ZOMBIE)
            continue;

        /* A blocked read owns an open-file reference for the duration of the
         * syscall even if another thread closes the numeric fd. */
        if (process->state == PLASMA_PROC_BLOCKED_IO &&
            process->blocked_pipe_object == object)
            return true;

        if (process == current) {
            if (plasma_fd_table_has_pipe_endpoint(plasma_runtime_fds,
                    PLASMA_RUNTIME_FD_COUNT, object, readers_only) ||
                plasma_fd_table_has_pipe_endpoint(plasma_low_runtime_fds,
                    10u, object, readers_only))
                return true;
            continue;
        }

        struct plasma_process *owner = plasma_thread_group_leader(process);
        if (owner == 0) owner = process;
        if (plasma_fd_table_has_pipe_endpoint(owner->runtime_fds,
                PLASMA_RUNTIME_FD_COUNT, object, readers_only) ||
            plasma_fd_table_has_pipe_endpoint(owner->low_runtime_fds,
                10u, object, readers_only))
            return true;
    }
    return false;
}

static bool plasma_pipe_has_live_reader(uint16_t object) {
    return plasma_pipe_scan_live(object, true);
}

static bool plasma_pipe_has_live_endpoint(uint16_t object) {
    return plasma_pipe_scan_live(object, false);
}

static void plasma_reclaim_pipe_if_unused(uint16_t object) {
    if (object >= PLASMA_PIPE_OBJECTS || !plasma_pipes[object].used) return;
    if (!plasma_pipe_has_live_endpoint(object))
        bytes_zero(&plasma_pipes[object], sizeof(plasma_pipes[object]));
}

static void plasma_reclaim_dead_pipes(void) {
    for (uint16_t object = 0; object < PLASMA_PIPE_OBJECTS; ++object)
        if (plasma_pipes[object].used)
            plasma_reclaim_pipe_if_unused(object);
}

static void plasma_close_cloexec_pipe_fds(void) {
    for (int fd = 0; fd < 10; ++fd) {
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 &&
            (entry->type == PLASMA_RT_PIPE_R || entry->type == PLASMA_RT_PIPE_W) &&
            (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
    for (int i = 0; i < PLASMA_RUNTIME_FD_COUNT; ++i) {
        const int fd = PLASMA_RUNTIME_FD_FIRST + i;
        struct plasma_runtime_fd *entry = plasma_runtime_fd(fd);
        if (entry != 0 &&
            (entry->type == PLASMA_RT_PIPE_R || entry->type == PLASMA_RT_PIPE_W) &&
            (entry->flags & PLASMA_SOCK_CLOEXEC) != 0)
            (void)plasma_close_runtime(fd);
    }
}

'''
    text = rep(text, exit_anchor, helpers + exit_anchor)

    # execve applies close-on-exec only after all new-image construction and
    # protection has succeeded. Scope this helper deliberately to pipes; other
    # runtime object classes get their own lifetime work rather than fake close.
    text = rep(text,
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n"
               "    linux_exec_set_return_state(image.entry, image.stack_pointer);\n",
               "    plasma_close_cloexec_pipe_fds();\n"
               "    bytes_zero(rootfs_open_files, sizeof(rootfs_open_files));\n"
               "    linux_exec_set_return_state(image.entry, image.stack_pointer);\n")

    # Whole-process exit drops every descriptor reference. The existing pipe EOF
    # finalizer already runs last-writer notification before this point.
    text = rep(text,
               "    plasma_wake_waiters_for(process);\n"
               "    serial_write(\"[linux:process] process exited pid=\");\n",
               "    plasma_reclaim_dead_pipes();\n"
               "    plasma_wake_waiters_for(process);\n"
               "    serial_write(\"[linux:process] process exited pid=\");\n")

    path.write_text(text, encoding="utf-8")
    print(f"Finalized transactional pipe2 + reusable pipe lifetime semantics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
