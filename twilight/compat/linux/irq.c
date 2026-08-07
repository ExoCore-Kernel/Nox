#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/errno.h>
#include <linux/interrupt.h>
#include <twilight/irq.h>

#define LINUX_IRQ_REGISTRATIONS 64u

struct linux_irq_registration {
    bool used;
    unsigned int irq;
    irq_handler_t handler;
    void *dev_id;
};

static struct linux_irq_registration registrations[LINUX_IRQ_REGISTRATIONS];
static volatile uint32_t registration_lock;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&registration_lock, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&registration_lock, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

static bool linux_irq_bridge(uint8_t irq, void *context) {
    struct linux_irq_registration *registration =
        (struct linux_irq_registration *)context;
    if (registration == 0 || !registration->used || registration->handler == 0) return false;
    const irqreturn_t result = registration->handler((int)irq, registration->dev_id);
    return result == IRQ_HANDLED || result == IRQ_WAKE_THREAD;
}

int request_irq(unsigned int irq,
                irq_handler_t handler,
                unsigned long flags,
                const char *name,
                void *dev_id) {
    if (handler == 0 || irq >= 16u || irq < 2u) return -EINVAL;
    if (!irq_core_is_initialized()) return -ENODEV;

    const uint64_t saved_flags = lock_irqsave();
    struct linux_irq_registration *free_slot = 0;

    for (size_t i = 0; i < LINUX_IRQ_REGISTRATIONS; ++i) {
        if (registrations[i].used) {
            if (registrations[i].irq == irq && registrations[i].dev_id == dev_id) {
                unlock_irqrestore(saved_flags);
                return -EBUSY;
            }
        } else if (free_slot == 0) {
            free_slot = &registrations[i];
        }
    }

    if (free_slot == 0) {
        unlock_irqrestore(saved_flags);
        return -ENOSPC;
    }

    *free_slot = (struct linux_irq_registration){
        .used = true,
        .irq = irq,
        .handler = handler,
        .dev_id = dev_id,
    };

    const uint32_t native_flags = (flags & IRQF_SHARED) != 0 ? TWILIGHT_IRQ_SHARED : 0u;
    if (!irq_request((uint8_t)irq, linux_irq_bridge, free_slot, native_flags, name)) {
        *free_slot = (struct linux_irq_registration){0};
        unlock_irqrestore(saved_flags);
        return -EBUSY;
    }

    unlock_irqrestore(saved_flags);
    return 0;
}

void free_irq(unsigned int irq, void *dev_id) {
    if (irq >= 16u || irq < 2u) return;

    const uint64_t saved_flags = lock_irqsave();
    for (size_t i = 0; i < LINUX_IRQ_REGISTRATIONS; ++i) {
        struct linux_irq_registration *registration = &registrations[i];
        if (!registration->used || registration->irq != irq || registration->dev_id != dev_id) continue;
        (void)irq_free((uint8_t)irq, linux_irq_bridge, registration);
        *registration = (struct linux_irq_registration){0};
        break;
    }
    unlock_irqrestore(saved_flags);
}

int request_threaded_irq(unsigned int irq,
                         irq_handler_t handler,
                         irq_handler_t thread_fn,
                         unsigned long flags,
                         const char *name,
                         void *dev_id) {
    /* A true sleeping IRQ thread requires the scheduler. Drivers that provide
     * only a hard-IRQ handler work now; threaded handlers fail explicitly. */
    if (thread_fn != 0) return -ENOSYS;
    return request_irq(irq, handler, flags, name, dev_id);
}
