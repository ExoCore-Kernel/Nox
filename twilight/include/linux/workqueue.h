#pragma once

#include <stdbool.h>
#include <stdint.h>

struct work_struct {
    void (*func)(struct work_struct *work);
    struct work_struct *next;
    volatile uint32_t state;
};

struct delayed_work {
    struct work_struct work;
    struct delayed_work *next_delayed;
    unsigned long due_jiffies;
    volatile uint32_t delayed_pending;
};

struct workqueue_struct {
    const char *name;
};

#define INIT_WORK(work, fn) do { \
    (work)->func = (fn); \
    (work)->next = 0; \
    (work)->state = 0u; \
} while (0)

#define INIT_DELAYED_WORK(dwork, fn) do { \
    INIT_WORK(&(dwork)->work, (fn)); \
    (dwork)->next_delayed = 0; \
    (dwork)->due_jiffies = 0; \
    (dwork)->delayed_pending = 0u; \
} while (0)

extern struct workqueue_struct *system_wq;

bool schedule_work(struct work_struct *work);
bool queue_work(struct workqueue_struct *wq, struct work_struct *work);
bool cancel_work_sync(struct work_struct *work);
void flush_work(struct work_struct *work);
void flush_scheduled_work(void);

bool schedule_delayed_work(struct delayed_work *work, unsigned long delay);
bool queue_delayed_work(struct workqueue_struct *wq,
                        struct delayed_work *work,
                        unsigned long delay);
bool cancel_delayed_work_sync(struct delayed_work *work);
