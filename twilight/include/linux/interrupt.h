#pragma once

#include <linux/types.h>

#define IRQF_SHARED       0x00000080ul
#define IRQF_TRIGGER_NONE 0x00000000ul
#define IRQF_ONESHOT      0x00002000ul

typedef enum {
    IRQ_NONE = 0,
    IRQ_HANDLED = 1,
    IRQ_WAKE_THREAD = 2,
} irqreturn_t;

#define IRQ_RETVAL(handled) ((handled) ? IRQ_HANDLED : IRQ_NONE)

typedef irqreturn_t (*irq_handler_t)(int irq, void *dev_id);

int request_irq(unsigned int irq,
                irq_handler_t handler,
                unsigned long flags,
                const char *name,
                void *dev_id);
void free_irq(unsigned int irq, void *dev_id);

int request_threaded_irq(unsigned int irq,
                         irq_handler_t handler,
                         irq_handler_t thread_fn,
                         unsigned long flags,
                         const char *name,
                         void *dev_id);

/* Twilight's current driver environment is uniprocessor and hard IRQ handlers
 * execute synchronously on the same CPU. Once control has returned from an IRQ
 * there cannot be another CPU still executing that handler, so the Linux
 * synchronize_irq contract reduces to a compiler/CPU ordering point for now. */
static inline void synchronize_irq(unsigned int irq) {
    (void)irq;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define local_irq_disable() __asm__ volatile ("cli" : : : "memory")
#define local_irq_enable()  __asm__ volatile ("sti" : : : "memory")

#define local_irq_save(flags) do { \
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory"); \
} while (0)

#define local_irq_restore(flags) do { \
    if (((flags) & (1ull << 9)) != 0) \
        __asm__ volatile ("sti" : : : "memory"); \
} while (0)
