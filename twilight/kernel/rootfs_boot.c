#include <stddef.h>
#include <stdint.h>

#include <limine.h>
#include <linux/printk.h>
#include <twilight/rootfs.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request rootfs_module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

static int rootfs_boot_init(void) {
    struct limine_module_response *response = rootfs_module_request.response;
    if (response == 0 || response->module_count == 0 || response->modules == 0) {
        pr_warn("Plasma rootfs: Limine supplied no modules");
        return 0;
    }

    struct limine_file *module = response->modules[0];
    if (module == 0 || module->address == 0 || module->size == 0) {
        pr_warn("Plasma rootfs: first Limine module is empty");
        return 0;
    }

    if (!rootfs_init(module->address, (size_t)module->size)) {
        pr_err("Plasma rootfs: invalid CPIO newc archive");
        return 0;
    }

    printk("[linux] Plasma rootfs mounted from Limine module: %zu entries, %llu bytes",
           rootfs_entry_count(), (unsigned long long)module->size);

    struct rootfs_node release;
    if (rootfs_lookup("/etc/nox-release", &release)) {
        printk("[linux] Plasma rootfs probe PASS: /etc/nox-release is readable (%zu bytes)",
               release.size);
    } else {
        pr_warn("Plasma rootfs mounted but /etc/nox-release is missing");
    }
    return 0;
}

/* This is infrastructure, not a self-test. It must execute even in Bash builds
 * where ordinary module_init() calls are intentionally isolated. */
static int (* const __twilight_rootfs_boot_initcall)(void)
__attribute__((used, section(".twilight_initcalls"))) = rootfs_boot_init;
