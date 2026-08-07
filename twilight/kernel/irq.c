#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <twilight/interrupts.h>
#include <twilight/irq.h>

#define IRQ_LINES 16u
#define IRQ_HANDLERS_PER_LINE 8u
#define X86_RFLAGS_IF (1ull << 9)

struct irq_slot {
    twilight_irq_handler_t handler;
    void *context;
    const char *name;
    uint32_t flags;
};

static struct irq_slot slots[IRQ_LINES][IRQ_HANDLERS_PER_LINE];
static bool initialized;
static volatile uint32_t irq_lock_word;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&irq_lock_word, 1u, __ATOMIC_ACQUIRE) != 0u) {
        __asm__ volatile ("pause");
    }
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&irq_lock_word, 0u, __ATOMIC_RELEASE);
    if ((flags & X86_RFLAGS_IF) != 0) __asm__ volatile ("sti" : : : "memory");
}

static bool line_has_handler(uint8_t irq) {
    for (size_t i = 0; i < IRQ_HANDLERS_PER_LINE; ++i) {
        if (slots[irq][i].handler != 0) return true;
    }
    return false;
}

bool irq_core_init(void) {
    const uint64_t flags = lock_irqsave();
    for (size_t irq = 0; irq < IRQ_LINES; ++irq) {
        for (size_t i = 0; i < IRQ_HANDLERS_PER_LINE; ++i) {
            slots[irq][i] = (struct irq_slot){0};
        }
    }
    initialized = true;
    unlock_irqrestore(flags);
    return true;
}

bool irq_core_is_initialized(void) {
    return initialized;
}

bool irq_request(uint8_t irq,
                 twilight_irq_handler_t handler,
                 void *context,
                 uint32_t flags,
                 const char *name) {
    if (!initialized || handler == 0 || irq >= IRQ_LINES) return false;

    /* PIT and PS/2 are still owned by their dedicated bring-up handlers. */
    if (irq < 2u) return false;

    const uint64_t saved_flags = lock_irqsave();

    bool already_used = false;
    bool all_shared = true;
    size_t free_index = IRQ_HANDLERS_PER_LINE;

    for (size_t i = 0; i < IRQ_HANDLERS_PER_LINE; ++i) {
        struct irq_slot *slot = &slots[irq][i];
        if (slot->handler == handler && slot->context == context) {
            unlock_irqrestore(saved_flags);
            return true;
        }
        if (slot->handler == 0) {
            if (free_index == IRQ_HANDLERS_PER_LINE) free_index = i;
            continue;
        }
        already_used = true;
        if ((slot->flags & TWILIGHT_IRQ_SHARED) == 0) all_shared = false;
    }

    if (free_index == IRQ_HANDLERS_PER_LINE ||
        (already_used && (((flags & TWILIGHT_IRQ_SHARED) == 0) || !all_shared))) {
        unlock_irqrestore(saved_flags);
        return false;
    }

    slots[irq][free_index].handler = handler;
    slots[irq][free_index].context = context;
    slots[irq][free_index].flags = flags;
    slots[irq][free_index].name = name;

    pic_unmask_irq(irq);
    unlock_irqrestore(saved_flags);
    return true;
}

bool irq_free(uint8_t irq, twilight_irq_handler_t handler, void *context) {
    if (!initialized || handler == 0 || irq >= IRQ_LINES || irq < 2u) return false;

    const uint64_t saved_flags = lock_irqsave();
    bool removed = false;

    for (size_t i = 0; i < IRQ_HANDLERS_PER_LINE; ++i) {
        struct irq_slot *slot = &slots[irq][i];
        if (slot->handler != handler || slot->context != context) continue;
        *slot = (struct irq_slot){0};
        removed = true;
        break;
    }

    if (removed && !line_has_handler(irq)) pic_mask_irq(irq);
    unlock_irqrestore(saved_flags);
    return removed;
}

void legacy_irq_dispatch(uint64_t irq_value) {
    const uint8_t irq = (uint8_t)irq_value;
    if (irq >= IRQ_LINES || irq < 2u) {
        if (irq < IRQ_LINES) pic_send_eoi(irq);
        return;
    }

    /* Interrupt gates arrive with IF cleared, so the slot array is stable on
     * the current single-CPU bring-up target while handlers execute. */
    if (initialized) {
        for (size_t i = 0; i < IRQ_HANDLERS_PER_LINE; ++i) {
            const struct irq_slot slot = slots[irq][i];
            if (slot.handler != 0) (void)slot.handler(irq, slot.context);
        }
    }

    pic_send_eoi(irq);
}
