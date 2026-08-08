#include <stdbool.h>

#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/linux_storage.h>
#include <twilight/pci.h>

void linux_pci_report_bindings(void);

#if defined(TWILIGHT_BUSYBOX_SELF_TEST) && TWILIGHT_BUSYBOX_SELF_TEST
/*
 * Interactive shell builds need native PCI enumeration to exist before exact
 * upstream module_init() entry points run. This ordinary initcall performs the
 * initial PCI setup and registers Twilight's built-in PCI drivers first.
 *
 * The linker then runs .twilight_driver_initcalls (exact upstream drivers),
 * followed by the post-driver report below, and only then enters Bash.
 *
 * Ordering:
 *   native Twilight PCI enumeration
 *     -> Linux pci_dev wrappers
 *     -> built-in PCI driver registration/probe
 *     -> exact upstream module_init() calls
 *     -> final binding inventory
 *     -> Bash
 */
static int linux_pci_shell_preflight(void) {
    if (!pci_is_initialized()) {
        pr_info("PCI shell preflight: initializing native Twilight PCI enumeration");
        if (!pci_init()) {
            pr_err("PCI shell preflight: native Twilight PCI initialization failed");
            return -ENODEV;
        }
        pr_info("PCI shell preflight: native PCI enumeration complete: %zu device(s)",
                pci_device_count());
    } else {
        pr_info("PCI shell preflight: native PCI already initialized: %zu device(s)",
                pci_device_count());
    }

    const int result = linux_pci_register_builtin_drivers();
    if (result != 0) {
        pr_err("PCI shell preflight: built-in driver registration failed: %d", result);
        linux_pci_report_bindings();
        return result;
    }

    if (linux_storage_publish_block_devices())
        pr_info("PCI shell preflight: block device(s) published after built-in probe");

    /* This inventory is intentionally the pre-upstream snapshot. A second
     * inventory is emitted after .twilight_driver_initcalls so test harnesses
     * can observe the final driver ownership rather than this intermediate
     * state. */
    linux_pci_report_bindings();
    pr_info("PCI shell preflight complete; running upstream Linux driver initcalls next");
    return 0;
}

static int (* const __twilight_pci_shell_preflight_initcall)(void)
__attribute__((used, section(".twilight_initcalls"))) = linux_pci_shell_preflight;

static int linux_pci_shell_post_driver_report(void) {
    if (linux_storage_publish_block_devices())
        pr_info("PCI shell post-driver: block device(s) published after upstream probe");

    pr_info("PCI shell post-driver: final binding inventory");
    linux_pci_report_bindings();
    pr_info("PCI shell driver initialization complete; entering interactive Linux userspace next");
    return 0;
}

static int (* const __twilight_pci_shell_post_driver_initcall)(void)
__attribute__((used, section(".twilight_post_driver_initcalls"))) =
    linux_pci_shell_post_driver_report;
#endif
