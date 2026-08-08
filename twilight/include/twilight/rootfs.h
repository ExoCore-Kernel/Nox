#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct rootfs_node {
    const char *name;
    const uint8_t *data;
    size_t size;
    uint32_t mode;
};

/* Mount a CPIO "newc" archive supplied by the bootloader. The archive remains
 * owned by Limine; Twilight keeps zero-copy pointers into it. */
bool rootfs_init(const void *archive, size_t size);
bool rootfs_available(void);
size_t rootfs_entry_count(void);

/* Paths may be absolute (/usr/bin/foo) or archive-style (usr/bin/foo). */
bool rootfs_lookup(const char *path, struct rootfs_node *out);
size_t rootfs_read(const struct rootfs_node *node, size_t offset,
                   void *buffer, size_t size);
