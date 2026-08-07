#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <twilight/linux_compat.h>
#include <twilight/timer.h>
#include <twilight/vmm.h>

static volatile unsigned irq_hits;
static volatile unsigned work_hits;
static volatile unsigned timer_hits;

static irqreturn_t runtime_irq_handler(int irq, void *dev_id) {
    if (irq != 5 || dev_id != (void *)&irq_hits) return IRQ_NONE;
    ++irq_hits;
    return IRQ_HANDLED;
}

static void runtime_work_handler(struct work_struct *work) {
    (void)work;
    ++work_hits;
}

static void runtime_timer_handler(struct timer_list *timer) {
    (void)timer;
    ++timer_hits;
}

bool linux_driver_runtime_self_test(void) {
    pr_info("driver runtime self-test: DMA mask");

    struct device dma_device = {0};
    if (dma_set_mask_and_coherent(&dma_device, DMA_BIT_MASK(32)) != 0) {
        pr_err("driver runtime self-test failed at DMA mask setup");
        return false;
    }

    pr_info("driver runtime self-test: coherent DMA");

    dma_addr_t coherent_dma = DMA_MAPPING_ERROR;
    uint8_t *coherent = (uint8_t *)dma_alloc_coherent(&dma_device,
                                                       8192u,
                                                       &coherent_dma,
                                                       GFP_KERNEL);
    if (coherent == 0 || coherent_dma == DMA_MAPPING_ERROR) {
        pr_err("driver runtime self-test failed at coherent DMA allocation");
        return false;
    }

    coherent[0] = 0x5au;
    coherent[8191] = 0xa5u;

    uint64_t translated = 0;
    if (!vmm_translate(vmm_kernel_space(), (uint64_t)(uintptr_t)coherent, &translated, 0)) {
        pr_err("driver runtime self-test failed translating coherent DMA virtual address");
        dma_free_coherent(&dma_device, 8192u, coherent, coherent_dma);
        return false;
    }
    if (translated != (uint64_t)coherent_dma) {
        pr_err("driver runtime self-test coherent DMA translation mismatch: cpu=%p translated=0x%llx dma=0x%llx",
               coherent,
               (unsigned long long)translated,
               (unsigned long long)coherent_dma);
        dma_free_coherent(&dma_device, 8192u, coherent, coherent_dma);
        return false;
    }

    dma_free_coherent(&dma_device, 8192u, coherent, coherent_dma);

    pr_info("driver runtime self-test: streaming DMA");

    uint8_t *streaming = (uint8_t *)kmalloc(5000u, GFP_KERNEL);
    if (streaming == 0) {
        pr_err("driver runtime self-test failed allocating streaming DMA buffer");
        return false;
    }

    const dma_addr_t streaming_dma = dma_map_single(&dma_device,
                                                     streaming,
                                                     5000u,
                                                     DMA_TO_DEVICE);
    if (dma_mapping_error(&dma_device, streaming_dma)) {
        pr_err("driver runtime self-test failed mapping streaming DMA buffer");
        kfree(streaming);
        return false;
    }

    dma_unmap_single(&dma_device, streaming_dma, 5000u, DMA_TO_DEVICE);
    kfree(streaming);

    pr_info("driver runtime self-test: shared legacy IRQ dispatch");

    irq_hits = 0;
    if (request_irq(5u,
                    runtime_irq_handler,
                    IRQF_SHARED,
                    "linux-runtime-selftest",
                    (void *)&irq_hits) != 0) {
        pr_err("driver runtime self-test failed registering IRQ5");
        return false;
    }

    const unsigned irq_hits_before = irq_hits;
    __asm__ volatile ("int $0x25");
    const unsigned irq_hits_after = irq_hits;
    free_irq(5u, (void *)&irq_hits);

    if (irq_hits_after <= irq_hits_before) {
        pr_err("driver runtime self-test IRQ5 dispatch did not reach handler");
        return false;
    }

    pr_info("driver runtime self-test: deferred workqueue");

    struct work_struct work;
    work_hits = 0;
    INIT_WORK(&work, runtime_work_handler);
    if (!schedule_work(&work)) {
        pr_err("driver runtime self-test failed queueing work item");
        return false;
    }
    linux_driver_runtime_poll();
    if (work_hits != 1u) {
        pr_err("driver runtime self-test workqueue callback count=%u", work_hits);
        return false;
    }

    pr_info("driver runtime self-test: timer");

    struct timer_list timer;
    timer_hits = 0;
    timer_setup(&timer, runtime_timer_handler, 0);
    (void)mod_timer(&timer, jiffies + 2ul);
    timer_sleep_ms(3u);
    linux_driver_runtime_poll();
    if (timer_hits != 1u) {
        pr_err("driver runtime self-test timer callback count=%u", timer_hits);
        (void)del_timer_sync(&timer);
        return false;
    }

    pr_info("driver runtime self-test: PASS");
    return true;
}
