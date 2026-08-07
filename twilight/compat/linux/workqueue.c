#include <stdbool.h>
#include <stdint.h>

#include <linux/jiffies.h>
#include <linux/workqueue.h>

static struct workqueue_struct system_wq_storage = { .name = "events" };
struct workqueue_struct *system_wq = &system_wq_storage;

static struct work_struct *work_head;
static struct work_struct *work_tail;
static struct delayed_work *delayed_head;
static volatile uint32_t work_lock;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&work_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&work_lock, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

static bool enqueue_locked(struct work_struct *work) {
    if (work == 0 || work->func == 0 || work->state != 0u) return false;
    work->state = 1u;
    work->next = 0;
    if (work_tail != 0) work_tail->next = work;
    else work_head = work;
    work_tail = work;
    return true;
}

bool schedule_work(struct work_struct *work) {
    const uint64_t flags = lock_irqsave();
    const bool result = enqueue_locked(work);
    unlock_irqrestore(flags);
    return result;
}

bool queue_work(struct workqueue_struct *wq, struct work_struct *work) {
    (void)wq;
    return schedule_work(work);
}

bool cancel_work_sync(struct work_struct *work) {
    if (work == 0) return false;
    const uint64_t flags = lock_irqsave();
    struct work_struct *previous = 0;
    struct work_struct *current = work_head;
    while (current != 0) {
        if (current == work) {
            if (previous != 0) previous->next = current->next;
            else work_head = current->next;
            if (work_tail == current) work_tail = previous;
            current->next = 0;
            current->state = 0u;
            unlock_irqrestore(flags);
            return true;
        }
        previous = current;
        current = current->next;
    }
    unlock_irqrestore(flags);
    return false;
}

void flush_work(struct work_struct *work) {
    if (work == 0) return;
    while (__atomic_load_n(&work->state, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
}

bool schedule_delayed_work(struct delayed_work *work, unsigned long delay) {
    if (work == 0 || work->work.func == 0) return false;
    if (delay == 0) return schedule_work(&work->work);

    const uint64_t flags = lock_irqsave();
    if (work->delayed_pending != 0u || work->work.state != 0u) {
        unlock_irqrestore(flags);
        return false;
    }
    work->due_jiffies = jiffies + delay;
    work->delayed_pending = 1u;
    work->next_delayed = delayed_head;
    delayed_head = work;
    unlock_irqrestore(flags);
    return true;
}

bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *work,
                        unsigned long delay) {
    (void)wq;
    return schedule_delayed_work(work, delay);
}

bool cancel_delayed_work_sync(struct delayed_work *work) {
    if (work == 0) return false;
    const uint64_t flags = lock_irqsave();
    struct delayed_work *previous = 0;
    struct delayed_work *current = delayed_head;
    while (current != 0) {
        if (current == work) {
            if (previous != 0) previous->next_delayed = current->next_delayed;
            else delayed_head = current->next_delayed;
            current->next_delayed = 0;
            current->delayed_pending = 0u;
            unlock_irqrestore(flags);
            return true;
        }
        previous = current;
        current = current->next_delayed;
    }
    unlock_irqrestore(flags);
    return cancel_work_sync(&work->work);
}

void linux_workqueue_poll(void) {
    for (;;) {
        struct work_struct *work = 0;
        const uint64_t flags = lock_irqsave();
        const unsigned long now = jiffies;

        struct delayed_work *previous = 0;
        struct delayed_work *current = delayed_head;
        while (current != 0) {
            struct delayed_work *next = current->next_delayed;
            if (time_after_eq(now, current->due_jiffies)) {
                if (previous != 0) previous->next_delayed = next;
                else delayed_head = next;
                current->next_delayed = 0;
                current->delayed_pending = 0u;
                (void)enqueue_locked(&current->work);
            } else {
                previous = current;
            }
            current = next;
        }

        if (work_head != 0) {
            work = work_head;
            work_head = work->next;
            if (work_tail == work) work_tail = 0;
            work->next = 0;
            work->state = 2u;
        }
        unlock_irqrestore(flags);

        if (work == 0) break;
        work->func(work);
        __atomic_store_n(&work->state, 0u, __ATOMIC_RELEASE);
    }
}

void flush_scheduled_work(void) {
    linux_workqueue_poll();
}
