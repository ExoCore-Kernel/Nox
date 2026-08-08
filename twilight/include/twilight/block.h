#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TWILIGHT_BLOCK_NAME_MAX 16u
#define TWILIGHT_BLOCK_MODEL_MAX 48u
#define TWILIGHT_BLOCK_MAX_DEVICES 16u

struct twilight_block_device;

typedef int (*twilight_block_read_fn)(void *context,
                                      uint64_t lba,
                                      uint32_t sectors,
                                      void *buffer);
typedef int (*twilight_block_write_fn)(void *context,
                                       uint64_t lba,
                                       uint32_t sectors,
                                       const void *buffer);

struct twilight_block_ops {
    twilight_block_read_fn read;
    twilight_block_write_fn write;
};

struct twilight_block_device {
    char name[TWILIGHT_BLOCK_NAME_MAX];
    char model[TWILIGHT_BLOCK_MODEL_MAX];
    uint64_t sector_count;
    uint32_t sector_size;
    bool writable;
    const struct twilight_block_ops *ops;
    void *context;
};

bool block_register_device(const char *name,
                           const char *model,
                           uint64_t sector_count,
                           uint32_t sector_size,
                           bool writable,
                           const struct twilight_block_ops *ops,
                           void *context,
                           struct twilight_block_device **out_device);

size_t block_device_count(void);
struct twilight_block_device *block_device_at(size_t index);
struct twilight_block_device *block_find_device(const char *name);

int block_read(struct twilight_block_device *device,
               uint64_t lba,
               uint32_t sectors,
               void *buffer);
int block_write(struct twilight_block_device *device,
                uint64_t lba,
                uint32_t sectors,
                const void *buffer);
