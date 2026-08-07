#pragma once

#include <linux/kernel.h>

struct timer_list {
    void (*function)(struct timer_list *timer);
    struct timer_list *next;
    unsigned long expires;
    volatile unsigned int pending;
};

#define timer_setup(timer, callback, flags) do { \
    (void)(flags); \
    (timer)->function = (callback); \
    (timer)->next = 0; \
    (timer)->expires = 0; \
    (timer)->pending = 0u; \
} while (0)

#define from_timer(var, callback_timer, timer_fieldname) \
    container_of((callback_timer), __typeof__(*(var)), timer_fieldname)

int mod_timer(struct timer_list *timer, unsigned long expires);
int del_timer(struct timer_list *timer);
int del_timer_sync(struct timer_list *timer);
int timer_pending(const struct timer_list *timer);
