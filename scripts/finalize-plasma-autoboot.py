#!/usr/bin/env python3
"""Boot the Plasma bring-up image graphically unless Limine requests a shell.

The generated compatibility process is GNU Bash. Keep it interactive so the
existing shell/debug path remains unchanged, but add a one-shot PROMPT_COMMAND
in the default boot mode. Bash runs that command before its first prompt.

Do not use xinit here. During bring-up xinit creates an early synchronization
pipe and can put both the shell parent and xinit child to sleep before either
Xorg or the Plasma client has exec'd, leaving the cooperative scheduler with no
runnable process. Instead reproduce the already-proven manual sequence exactly:
start Xorg in the background, set DISPLAY/runtime environment, then exec the
session bus in the original shell process.

For the first Plasma client, invoke the known-good musl loader directly with
/usr/bin/startplasma-x11 as its target.  The rootfs definitely contains the
launcher, but the kernel-side exec path currently reports ENOENT for that one
binary.  Direct loader invocation keeps the real Plasma executable and its
normal userspace dynamic-link process while bypassing only that failing kernel
PT_INTERP/exec transition.  The exec finalizer also logs exact path-resolution
failures so the underlying ABI issue remains visible instead of hidden.

During bring-up, force Alpine's compiled Qt6 plugin directory and enable Qt
plugin diagnostics.  Plasma has reached ksplashqml but Qt currently reports an
empty platform-plugin search location.  Alpine installs libqxcb.so below
/usr/lib/qt6/plugins/platforms; setting QT_PLUGIN_PATH both gives the intended
location explicitly and makes QT_DEBUG_PLUGINS explain any remaining dlopen or
VFS failure rather than collapsing it into a generic "xcb not found" message.

Passing `nox.shell=1` (or the compatibility alias `boot=shell`) on Limine's
kernel command line suppresses PROMPT_COMMAND and leaves the normal nox# shell.
If Plasma exits, PROMPT_COMMAND has already unset itself, so Bash will not
relaunch the GUI forever.
"""
from __future__ import annotations

import pathlib
import sys


def rep(text: str, old: str, new: str, count: int = 1) -> str:
    if text.count(old) < count:
        raise RuntimeError(f"expected Plasma autoboot fragment not found: {old[:180]!r}")
    return text.replace(old, new, count)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} GENERATED_BASH_C", file=sys.stderr)
        return 2

    path = pathlib.Path(sys.argv[1])
    text = path.read_text(encoding="utf-8")

    text = rep(
        text,
        "extern void linux_syscall_entry(void);\n",
        "extern void linux_syscall_entry(void);\n"
        "extern bool twilight_boot_shell_requested(void);\n",
    )

    env_anchor = '    const char env3[] = "PS1=nox# ";\n'
    env_block = r'''    const char env3[] = "PS1=nox# ";
    const char env4_auto[] =
        "PROMPT_COMMAND=unset PROMPT_COMMAND; export HOME=/tmp/runtime-root; "
        "/usr/bin/Xorg :0 -retro -extension GLX -nolisten tcp -novtswitch "
        "-sharevts -logfile /dev/null & export DISPLAY=:0; "
        "export XDG_RUNTIME_DIR=/tmp/runtime-root; export KWIN_COMPOSE=N; "
        "export QT_PLUGIN_PATH=/usr/lib/qt6/plugins; export QT_QPA_PLATFORM=xcb; "
        "export QT_DEBUG_PLUGINS=1; "
        "exec /usr/bin/dbus-run-session /lib/ld-musl-x86_64.so.1 "
        "/usr/bin/startplasma-x11";
    const char env4_shell[] = "PROMPT_COMMAND=";
    const char *env4 = twilight_boot_shell_requested() ? env4_shell : env4_auto;
'''
    text = rep(text, env_anchor, env_block)

    text = rep(
        text,
        "    uint64_t argv0_va=0, argv1_va=0, env0_va=0, env1_va=0, env2_va=0, env3_va=0;\n",
        "    uint64_t argv0_va=0, argv1_va=0, env0_va=0, env1_va=0, env2_va=0, env3_va=0, env4_va=0;\n",
    )

    text = rep(
        text,
        "        !push_stack_string(&cursor, env3, &env3_va) ||\n"
        "        !push_stack_string(&cursor, env2, &env2_va) ||\n",
        "        !push_stack_string(&cursor, env4, &env4_va) ||\n"
        "        !push_stack_string(&cursor, env3, &env3_va) ||\n"
        "        !push_stack_string(&cursor, env2, &env2_va) ||\n",
    )

    text = rep(
        text,
        "    const size_t table_words = 1u + 3u + 5u + aux_words;\n",
        "    const size_t table_words = 1u + 3u + 6u + aux_words;\n",
    )

    text = rep(
        text,
        "    if (!stack_u64(p, env3_va)) return false; p += 8;\n"
        "    if (!stack_u64(p, 0)) return false; p += 8;\n",
        "    if (!stack_u64(p, env3_va)) return false; p += 8;\n"
        "    if (!stack_u64(p, env4_va)) return false; p += 8;\n"
        "    if (!stack_u64(p, 0)) return false; p += 8;\n",
    )

    trace_anchor = (
        "    shell_active = true;\n"
        "    shell_exit_seen = false;\n"
        "    shell_exit_status = -1;\n"
    )
    trace_block = trace_anchor + (
        "    if (twilight_boot_shell_requested())\n"
        "        trace(\"boot mode: interactive shell requested by Limine (nox.shell=1)\");\n"
        "    else\n"
        "        trace(\"boot mode: automatic Xorg + D-Bus + Plasma via musl loader\");\n"
    )
    text = rep(text, trace_anchor, trace_block)

    path.write_text(text, encoding="utf-8")
    print(f"Finalized Plasma graphical autoboot + forced Qt6 XCB plugin diagnostics: {path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
