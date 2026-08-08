#include <stdbool.h>

#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/interrupts.h>
#include <twilight/ioapic.h>
#include <twilight/irq.h>
#include <twilight/linux_compat.h>
#include <twilight/linux_storage.h>

void linux_workqueue_poll(void);
void linux_timer_poll(void);

extern int (*__twilight_initcalls_start[])(void);
extern int (*__twilight_initcalls_end[])(void);

static bool runtime_initialized;
static bool runtime_initializing;

static bool run_builtin_initcalls(void) {
    for (int (**entry)(void) = __twilight_initcalls_start;
         entry < __twilight_initcalls_end;
         ++entry) {
        if (*entry == 0) continue;
        const int result = (*entry)();
        if (result != 0) {
            pr_err("Linux module initcall failed: %d", result);
            return false;
        }
    }
    return true;
}

static void upgrade_interrupt_controller(void) {
    if (ioapic_is_active()) return;

    const bool was_enabled = interrupts_are_enabled();
    interrupts_disable();

    if (ioapic_init()) {
        /* PIT was already proven through the conservative PIC/virtual-wire
         * bring-up. Keep its IRQ live immediately after switching controllers;
         * PS/2 and PCI lines are unmasked later by their normal owners. */
        pic_unmask_irq(0);
        pr_info("ACPI MADT IOAPIC routing active; Local APIC native mode ready for modern PCI interrupts");
    } else {
        pr_info("IOAPIC/native Local APIC unavailable; retaining legacy PIC interrupt routing");
    }

    if (was_enabled) interrupts_enable();
}

bool linux_driver_runtime_init(void) {
    if (runtime_initialized) return true;
    if (runtime_initializing) return false;
    runtime_initializing = true;

    /* Upgrade only after the PIT has proven that the conservative early-boot
     * route works. This keeps a reliable PIC fallback while giving Linux PCI
     * drivers IOAPIC routing before their initcalls/probes run. */
    upgrade_interrupt_controller();

    if (!irq_core_init()) {
        runtime_initializing = false;
        return false;
    }

#if !defined(TWILIGHT_BUSYBOX_SELF_TEST) || !TWILIGHT_BUSYBOX_SELF_TEST
    if (!linux_driver_runtime_self_test()) {
        pr_err("driver runtime self-test failed");
        runtime_initializing = false;
        return false;
    }

    pr_info("driver runtime self-test: IRQ, coherent/streaming DMA, workqueues and timers ready");
#endif

    if (!run_builtin_initcalls()) {
        runtime_initializing = false;
        return false;
    }

    const int result = linux_pci_register_builtin_drivers();
    if (result != 0) {
        pr_err("built-in Linux PCI driver registration failed: %d", result);
        runtime_initializing = false;
        return false;
    }

    if (linux_storage_publish_block_devices()) {
        pr_info("Linux storage bridge published block device(s) after PCI probe");
    }

    runtime_initialized = true;
    runtime_initializing = false;
    pr_info("built-in Linux PCI drivers registered after interrupt bring-up");

#if !defined(TWILIGHT_BUSYBOX_SELF_TEST) || !TWILIGHT_BUSYBOX_SELF_TEST
    if (linux_net_device_count() != 0) {
        pr_info("Linux network core: %zu Ethernet device(s) registered; starting end-to-end ARP test",
                linux_net_device_count());
        if (!linux_net_run_arp_self_test()) {
            pr_warn("Linux Ethernet ARP self-test did not complete; driver runtime remains active for diagnostics");
        }
    }
#endif

    return true;
}

bool linux_driver_runtime_is_initialized(void) {
    return runtime_initialized;
}

void linux_driver_runtime_poll(void) {
    if (!runtime_initialized && !runtime_initializing) return;

    linux_napi_poll();
    linux_timer_poll();
    linux_workqueue_poll();
    linux_napi_poll();
}
