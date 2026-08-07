#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
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
    struct device dma_device = {0};
    if (dma_set_mask_and_coherent(&dma_device, DMA_BIT_MASK(32)) != 0) return false;

    dma_addr_t coherent_dma = DMA_MAPPING_ERROR;
    uint8_t *coherent = (uint8_t *)dma_alloc_coherent(&dma_device,
                                                       8192u,
                                                       &coherent_dma,
                                                       GFP_KERNEL);
    if (coherent == 0 || coherent_dma == DMA_MAPPING_ERROR) return false;
    coherent[0] = 0x5au;
    coherent[8191] = 0xa5u;

    uint64_t translated = 0;
    if (!vmm_translate(vmm_kernel_space(), (uint64_t)(uintptr_t)coherent, &translated, 0) ||
        translated != (uint64_t)coherent_dma) {
        dma_free_coherent(&dma_device, 8192u, coherent, coherent_dma);
        return false;
    }
    dma_free_coherent(&dma_device, 8192u, coherent, coherent_dma);

    uint8_t *streaming = (uint8_t *)kmalloc(5000u, GFP_KERNEL);
    if (streaming == 0) return false;
    const dma_addr_t streaming_dma = dma_map_single(&dma_device,
                                                     streaming,
                                                     5000u,
                                                     DMA_TO_DEVICE);
    if (dma_mapping_error(&dma_device, streaming_dma)) {
        kfree(streaming);
        return false;
    }
    dma_unmap_single(&dma_device, streaming_dma, 5000u, DMA_TO_DEVICE);
    kfree(streaming);

    irq_hits = 0;
    if (request_irq(5u, runtime_irq_handler, 0, "linux-runtime-selftest", (void *)&irq_hits) != 0)
        return false;
    __asm__ volatile ("int $0x25");
    free_irq(5u, (void *)&irq_hits);
    if (irq_hits != 1u) return false;

    struct work_struct work;
    work_hits = 0;
    INIT_WORK(&work, runtime_work_handler);
    if (!schedule_work(&work)) return false;
    linux_driver_runtime_poll();
    if (work_hits != 1u) return false;

    struct timer_list timer;
    timer_hits = 0;
    timer_setup(&timer, runtime_timer_handler, 0);
    (void)mod_timer(&timer, jiffies + 2ul);
    timer_sleep_ms(3u);
    linux_driver_runtime_poll();
    if (timer_hits != 1u) return false;

    return true;
}
