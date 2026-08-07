#pragma once

#include <linux/device.h>
#include <linux/io.h>

struct attribute_group {
    const void *unused;
};

extern const struct attribute_group *pvpanic_dev_groups[];

int devm_pvpanic_probe(struct device *dev, void __iomem *base);
void pvpanic_twilight_panic_notify(void);
