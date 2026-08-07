#include <stdbool.h>

#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/irq.h>
#include <twilight/linux_compat.h>

void linux_workqueue_poll(void);
void linux_timer_poll(void);

bool linux_driver_runtime_init(void) {
    if (!irq_core_init()) return false;

    if (!linux_driver_runtime_self_test()) {
        pr_err("driver runtime self-test failed");
        return false;
    }

    pr_info("driver runtime self-test: IRQ, coherent/streaming DMA, workqueues and timers ready");

    const int result = linux_pci_register_builtin_drivers();
    if (result != 0) {
        pr_err("built-in Linux PCI driver registration failed: %d", result);
        return false;
    }

    pr_info("built-in Linux PCI drivers registered after interrupt bring-up");
    return true;
}

void linux_driver_runtime_poll(void) {
    linux_timer_poll();
    linux_workqueue_poll();
}
