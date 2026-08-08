#include <stdbool.h>

#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/linux_storage.h>
#include <twilight/pci.h>

void linux_pci_report_bindings(void);

#if defined(TWILIGHT_BUSYBOX_SELF_TEST) && TWILIGHT_BUSYBOX_SELF_TEST
/*
 * Interactive shell builds deliberately expose the shell as the only named
 * module_init() subsection. This ordinary initcall runs immediately before the
 * shell initcall so native PCI enumeration and Linux driver matching/probe are
 * complete and observable before Bash blocks the rest of
 * linux_driver_runtime_init().
 *
 * The ordering is intentional:
 *   native Twilight PCI enumeration
 *     -> Linux pci_dev wrappers
 *     -> built-in Linux driver registration/probe
 *     -> binding inventory
 *     -> Bash
 *
 * linux_pci_register_builtin_drivers() is idempotent: when the runtime reaches
 * its normal registration pass after the user exits, already registered
 * drivers are simply ignored.
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
        pr_info("PCI shell preflight: block device(s) published after probe");

    linux_pci_report_bindings();
    pr_info("PCI shell preflight complete; entering interactive Linux userspace next");
    return 0;
}

static int (* const __twilight_pci_shell_preflight_initcall)(void)
__attribute__((used, section(".twilight_initcalls"))) = linux_pci_shell_preflight;
#endif