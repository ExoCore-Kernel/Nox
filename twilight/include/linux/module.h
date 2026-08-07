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

#define THIS_MODULE ((void *)0)

#define __init
#define __exit
#define __maybe_unused __attribute__((unused))

#define module_init(fn) \
    static int (* const __twilight_initcall_##fn)(void) \
    __attribute__((used, section(".twilight_initcalls"))) = (fn)

#define module_exit(fn) \
    static void (* const __twilight_exitcall_##fn)(void) \
    __attribute__((used, section(".twilight_exitcalls"))) = (fn)
