#include <stdbool.h>

#include <linux/pci.h>
#include <linux/printk.h>
#include <twilight/irq.h>
#include <twilight/linux_compat.h>

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
