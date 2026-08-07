#pragma once

struct device {
    void *driver_data;
    const char *init_name;
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
