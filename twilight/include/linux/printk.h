#pragma once

#include <stdarg.h>

#define KERN_EMERG   ""
#define KERN_ALERT   ""
#define KERN_CRIT    ""
#define KERN_ERR     ""
#define KERN_WARNING ""
#define KERN_NOTICE  ""
#define KERN_INFO    ""
#define KERN_DEBUG   ""

int printk(const char *format, ...);
int vprintk(const char *format, va_list arguments);

#define pr_emerg(format, ...) printk("[linux:emerg] " format, ##__VA_ARGS__)
#define pr_alert(format, ...) printk("[linux:alert] " format, ##__VA_ARGS__)
#define pr_crit(format, ...)  printk("[linux:crit] " format, ##__VA_ARGS__)
#define pr_err(format, ...)   printk("[linux:error] " format, ##__VA_ARGS__)
#define pr_warn(format, ...)  printk("[linux:warn] " format, ##__VA_ARGS__)
#define pr_notice(format, ...) printk("[linux:notice] " format, ##__VA_ARGS__)
#define pr_info(format, ...)  printk("[linux] " format, ##__VA_ARGS__)
#define pr_debug(format, ...) printk("[linux:debug] " format, ##__VA_ARGS__)
