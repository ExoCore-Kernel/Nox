#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/slab.h>

#define DEVRES_MAX 256u

struct devres_entry {
    bool used;
    struct device *dev;
    devm_action_fn action;
    void *data;
    uint64_t sequence;
};

static struct devres_entry entries[DEVRES_MAX];
static uint64_t next_sequence;
static volatile uint32_t devres_lock;

static uint64_t lock_irqsave(void) {
    uint64_t flags;
    __asm__ volatile ("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    while (__atomic_exchange_n(&devres_lock, 1u, __ATOMIC_ACQUIRE) != 0u)
        __asm__ volatile ("pause");
    return flags;
}

static void unlock_irqrestore(uint64_t flags) {
    __atomic_store_n(&devres_lock, 0u, __ATOMIC_RELEASE);
    if ((flags & (1ull << 9)) != 0) __asm__ volatile ("sti" : : : "memory");
}

static void devm_kfree_action(void *data) {
    kfree(data);
}

int devm_add_action_or_reset(struct device *dev, devm_action_fn action, void *data) {
    if (dev == 0 || action == 0) return -EINVAL;

    const uint64_t flags = lock_irqsave();
    for (size_t i = 0; i < DEVRES_MAX; ++i) {
        if (entries[i].used) continue;
        entries[i] = (struct devres_entry){
            .used = true,
            .dev = dev,
            .action = action,
            .data = data,
            .sequence = ++next_sequence,
        };
        unlock_irqrestore(flags);
        return 0;
    }
    unlock_irqrestore(flags);

    action(data);
    return -ENOMEM;
}

void *devm_kmalloc(struct device *dev, size_t size, gfp_t flags) {
    (void)flags;
    if (dev == 0 || size == 0) return 0;
    void *pointer = kmalloc(size, GFP_KERNEL);
    if (pointer == 0) return 0;
    if (devm_add_action_or_reset(dev, devm_kfree_action, pointer) != 0) return 0;
    return pointer;
}

void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags) {
    void *pointer = devm_kmalloc(dev, size, flags);
    if (pointer == 0) return 0;
    uint8_t *bytes = (uint8_t *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
    return pointer;
}

void devm_kfree(struct device *dev, void *pointer) {
    if (dev == 0 || pointer == 0) return;

    const uint64_t flags = lock_irqsave();
    for (size_t i = 0; i < DEVRES_MAX; ++i) {
        if (!entries[i].used || entries[i].dev != dev ||
            entries[i].action != devm_kfree_action || entries[i].data != pointer) {
            continue;
        }
        entries[i] = (struct devres_entry){0};
        unlock_irqrestore(flags);
        kfree(pointer);
        return;
    }
    unlock_irqrestore(flags);
}

void devm_release_all(struct device *dev) {
    if (dev == 0) return;

    for (;;) {
        devm_action_fn action = 0;
        void *data = 0;
        uint64_t best_sequence = 0;
        size_t best = DEVRES_MAX;

        const uint64_t flags = lock_irqsave();
        for (size_t i = 0; i < DEVRES_MAX; ++i) {
            if (!entries[i].used || entries[i].dev != dev) continue;
            if (entries[i].sequence >= best_sequence) {
                best_sequence = entries[i].sequence;
                best = i;
            }
        }
        if (best != DEVRES_MAX) {
            action = entries[best].action;
            data = entries[best].data;
            entries[best] = (struct devres_entry){0};
        }
        unlock_irqrestore(flags);

        if (action == 0) break;
        action(data);
    }
}
