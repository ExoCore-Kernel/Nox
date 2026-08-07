#include <stdbool.h>

#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/irq.h>
#include <twilight/linux_compat.h>

void linux_workqueue_poll(void);
void linux_timer_poll(void);

static bool runtime_initialized;
static bool runtime_initializing;

bool linux_driver_runtime_init(void) {
    if (runtime_initialized) return true;
    if (runtime_initializing) return false;
    runtime_initializing = true;

    if (!irq_core_init()) {
        runtime_initializing = false;
        return false;
    }

    if (!linux_driver_runtime_self_test()) {
        pr_err("driver runtime self-test failed");
        runtime_initializing = false;
        return false;
    }

    pr_info("driver runtime self-test: IRQ, coherent/streaming DMA, workqueues and timers ready");

    const int result = linux_pci_register_builtin_drivers();
    if (result != 0) {
        pr_err("built-in Linux PCI driver registration failed: %d", result);
        runtime_initializing = false;
        return false;
    }

    runtime_initialized = true;
    runtime_initializing = false;
    pr_info("built-in Linux PCI drivers registered after interrupt bring-up");
    return true;
}

bool linux_driver_runtime_is_initialized(void) {
    return runtime_initialized;
}

void linux_driver_runtime_poll(void) {
    if (!runtime_initialized) return;
    linux_timer_poll();
    linux_workqueue_poll();
}
