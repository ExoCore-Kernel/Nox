#include <stdbool.h>
#include <stdint.h>

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/slab.h>

#include "pvpanic.h"

#define PVPANIC_PANICKED      (1u << 0)
#define PVPANIC_CRASH_LOADED  (1u << 1)
#define PVPANIC_SHUTDOWN      (1u << 2)

struct pvpanic_instance {
    void __iomem *base;
    uint8_t capability;
};

static struct pvpanic_instance *active_instance;
static const struct attribute_group empty_group = {0};
const struct attribute_group *pvpanic_dev_groups[] = {
    &empty_group,
    0,
};

int devm_pvpanic_probe(struct device *dev, void __iomem *base) {
    if (dev == 0 || base == 0) return -EINVAL;

    struct pvpanic_instance *instance =
        (struct pvpanic_instance *)devm_kzalloc(dev, sizeof(*instance), GFP_KERNEL);
    if (instance == 0) return -ENOMEM;

    instance->base = base;
    instance->capability = (uint8_t)(readb(base) &
        (PVPANIC_PANICKED | PVPANIC_CRASH_LOADED | PVPANIC_SHUTDOWN));
    if (instance->capability == 0u) return -ENODEV;

    dev_set_drvdata(dev, instance);
    active_instance = instance;

    dev_info(dev,
             "upstream Linux pvpanic-pci driver active; capability=0x%x",
             (unsigned)instance->capability);
    return 0;
}

void pvpanic_twilight_panic_notify(void) {
    struct pvpanic_instance *instance = active_instance;
    if (instance == 0 || instance->base == 0) return;
    if ((instance->capability & PVPANIC_PANICKED) == 0u) return;

    /* This is the same hardware event the Linux pvpanic core emits from its
     * panic notifier: bit 0 tells QEMU that the guest has panicked. */
    writeb(PVPANIC_PANICKED, instance->base);
}
