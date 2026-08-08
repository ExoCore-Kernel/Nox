#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/errno.h>
#include <linux/printk.h>
#include <twilight/block.h>

static struct twilight_block_device devices[TWILIGHT_BLOCK_MAX_DEVICES];
static size_t device_count;

static void copy_text(char *out, size_t capacity, const char *text) {
    if (out == 0 || capacity == 0) return;
    size_t i = 0;
    if (text != 0) {
        for (; i + 1u < capacity && text[i] != '\0'; ++i) out[i] = text[i];
    }
    out[i] = '\0';
}

static bool text_equal(const char *a, const char *b) {
    if (a == 0 || b == 0) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

bool block_register_device(const char *name,
                           const char *model,
                           uint64_t sector_count,
                           uint32_t sector_size,
                           bool writable,
                           const struct twilight_block_ops *ops,
                           void *context,
                           struct twilight_block_device **out_device) {
    if (name == 0 || name[0] == '\0' || sector_count == 0 || sector_size == 0 ||
        ops == 0 || ops->read == 0 || context == 0) {
        return false;
    }

    for (size_t i = 0; i < device_count; ++i) {
        if (text_equal(devices[i].name, name)) return false;
    }
    if (device_count >= TWILIGHT_BLOCK_MAX_DEVICES) return false;

    struct twilight_block_device *device = &devices[device_count++];
    *device = (struct twilight_block_device){0};
    copy_text(device->name, sizeof(device->name), name);
    copy_text(device->model, sizeof(device->model), model != 0 ? model : "block device");
    device->sector_count = sector_count;
    device->sector_size = sector_size;
    device->writable = writable && ops->write != 0;
    device->ops = ops;
    device->context = context;

    if (out_device != 0) *out_device = device;

    printk("[block] %s registered: model=%s sectors=%llu sector-size=%u writable=%s",
           device->name,
           device->model,
           (unsigned long long)device->sector_count,
           device->sector_size,
           device->writable ? "yes" : "no");
    return true;
}

size_t block_device_count(void) {
    return device_count;
}

struct twilight_block_device *block_device_at(size_t index) {
    return index < device_count ? &devices[index] : 0;
}

struct twilight_block_device *block_find_device(const char *name) {
    if (name == 0) return 0;
    for (size_t i = 0; i < device_count; ++i) {
        if (text_equal(devices[i].name, name)) return &devices[i];
    }
    return 0;
}

static int validate_io(struct twilight_block_device *device,
                       uint64_t lba,
                       uint32_t sectors,
                       const void *buffer) {
    if (device == 0 || buffer == 0 || sectors == 0) return -EINVAL;
    if (lba >= device->sector_count) return -ERANGE;
    if ((uint64_t)sectors > device->sector_count - lba) return -ERANGE;
    return 0;
}

int block_read(struct twilight_block_device *device,
               uint64_t lba,
               uint32_t sectors,
               void *buffer) {
    const int rc = validate_io(device, lba, sectors, buffer);
    if (rc != 0) return rc;
    if (device->ops == 0 || device->ops->read == 0) return -ENOSYS;
    return device->ops->read(device->context, lba, sectors, buffer);
}

int block_write(struct twilight_block_device *device,
                uint64_t lba,
                uint32_t sectors,
                const void *buffer) {
    const int rc = validate_io(device, lba, sectors, buffer);
    if (rc != 0) return rc;
    if (!device->writable || device->ops == 0 || device->ops->write == 0) return -EROFS;
    return device->ops->write(device->context, lba, sectors, buffer);
}
