#pragma once

#include <stddef.h>

#include <linux/printk.h>
#include <linux/types.h>

struct device {
    void *driver_data;
    const char *init_name;

    u64 dma_mask_storage;
    u64 *dma_mask;
    u64 coherent_dma_mask;
};

static inline void dev_set_drvdata(struct device *dev, void *data) {
    if (dev != 0) dev->driver_data = data;
}

static inline void *dev_get_drvdata(const struct device *dev) {
    return dev != 0 ? dev->driver_data : 0;
}

static inline const char *dev_name(const struct device *dev) {
    return (dev != 0 && dev->init_name != 0) ? dev->init_name : "device";
}

#define dev_err(dev, format, ...) \
    printk("[linux:error] %s: " format, dev_name(dev), ##__VA_ARGS__)
#define dev_warn(dev, format, ...) \
    printk("[linux:warn] %s: " format, dev_name(dev), ##__VA_ARGS__)
#define dev_info(dev, format, ...) \
    printk("[linux] %s: " format, dev_name(dev), ##__VA_ARGS__)
#define dev_dbg(dev, format, ...) \
    printk("[linux:debug] %s: " format, dev_name(dev), ##__VA_ARGS__)

typedef void (*devm_action_fn)(void *data);

void *devm_kmalloc(struct device *dev, size_t size, gfp_t flags);
void *devm_kzalloc(struct device *dev, size_t size, gfp_t flags);
void devm_kfree(struct device *dev, void *pointer);
int devm_add_action_or_reset(struct device *dev, devm_action_fn action, void *data);
void devm_release_all(struct device *dev);
