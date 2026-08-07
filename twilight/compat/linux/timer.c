#include <stdint.h>

#include <linux/jiffies.h>
#include <linux/timer.h>

static struct timer_list *timer_head;
static volatile uint32_t timer_lock;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&timer_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&timer_lock, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

int mod_timer(struct timer_list *timer, unsigned long expires) {
    if (timer == 0 || timer->function == 0) return 0;
    const uint64_t flags = lock_irqsave();
    const int was_pending = timer->pending != 0u;
    if (!was_pending) {
        timer->next = timer_head;
        timer_head = timer;
        timer->pending = 1u;
    }
    timer->expires = expires;
    unlock_irqrestore(flags);
    return was_pending;
}

int del_timer(struct timer_list *timer) {
    if (timer == 0) return 0;
    const uint64_t flags = lock_irqsave();
    struct timer_list *previous = 0;
    struct timer_list *current = timer_head;
    while (current != 0) {
        if (current == timer) {
            if (previous != 0) previous->next = current->next;
            else timer_head = current->next;
            current->next = 0;
            current->pending = 0u;
            unlock_irqrestore(flags);
            return 1;
        }
        previous = current;
        current = current->next;
    }
    unlock_irqrestore(flags);
    return 0;
}

int del_timer_sync(struct timer_list *timer) {
    return del_timer(timer);
}

int timer_pending(const struct timer_list *timer) {
    return timer != 0 && timer->pending != 0u;
}

void linux_timer_poll(void) {
    for (;;) {
        struct timer_list *due = 0;
        const uint64_t flags = lock_irqsave();
        const unsigned long now = jiffies;

        struct timer_list *previous = 0;
        struct timer_list *current = timer_head;
        while (current != 0) {
            if (time_after_eq(now, current->expires)) {
                if (previous != 0) previous->next = current->next;
                else timer_head = current->next;
                current->next = 0;
                current->pending = 0u;
                due = current;
                break;
            }
            previous = current;
            current = current->next;
        }
        unlock_irqrestore(flags);

        if (due == 0) break;
        due->function(due);
    }
}
