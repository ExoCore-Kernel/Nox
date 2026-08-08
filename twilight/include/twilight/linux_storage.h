#pragma once

#include <stdbool.h>

/* Publish disks discovered by Linux/libata drivers through Twilight's generic
 * block-device layer. Safe to call after built-in Linux PCI probes complete. */
bool linux_storage_publish_block_devices(void);
