#pragma once

#define MODULE_LICENSE(value)
#define MODULE_AUTHOR(value)
#define MODULE_DESCRIPTION(value)
#define MODULE_VERSION(value)
#define MODULE_ALIAS(value)
#define MODULE_DEVICE_TABLE(type, name)
#define MODULE_PARM_DESC(name, description)
#define EXPORT_SYMBOL_GPL(symbol)
#define EXPORT_SYMBOL(symbol)

/* Built-in Twilight drivers have no load-time module parameter parser yet.
 * Keep the declarations source-compatible and retain their compiled defaults. */
#define module_param(name, type, permissions)
#define module_param_named(name, value, type, permissions)
#define module_param_array(name, type, nump, permissions)

#define THIS_MODULE ((void *)0)

#define __init
#define __exit
#define __maybe_unused __attribute__((unused))

/* busybox_shell.c uses the native x86-64 Linux readlink syscall number. Keep
 * this guarded because the older BusyBox echo harness defines the same token
 * itself. This can move to a wider Linux UAPI syscall-number header later. */
#if defined(TWILIGHT_BUSYBOX_SELF_TEST) && TWILIGHT_BUSYBOX_SELF_TEST
#ifndef SYS_READLINK
#define SYS_READLINK 89ull
#endif
#endif

/* In a BusyBox boot we no longer want every bring-up/self-test initcall to run
 * before the interactive shell. Give each initcall its own section instead;
 * the linker deliberately exposes only busybox_shell_init to the runtime's
 * initcall range. Normal diagnostic/driver builds keep the original section. */
#if defined(TWILIGHT_BUSYBOX_SELF_TEST) && TWILIGHT_BUSYBOX_SELF_TEST
#define module_init(fn) \
    static int (* const __twilight_initcall_##fn)(void) \
    __attribute__((used, section(".twilight_initcalls." #fn))) = (fn)
#else
#define module_init(fn) \
    static int (* const __twilight_initcall_##fn)(void) \
    __attribute__((used, section(".twilight_initcalls"))) = (fn)
#endif

#define module_exit(fn) \
    static void (* const __twilight_exitcall_##fn)(void) \
    __attribute__((used, section(".twilight_exitcalls"))) = (fn)
