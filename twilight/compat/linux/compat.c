#include <stddef.h>
#include <stdint.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <twilight/linux_compat.h>
#include <twilight/pci.h>

struct compat_test_node {
    uint32_t value;
    struct list_head link;
};

bool linux_compat_self_test(void) {
    uint8_t *zeroed = (uint8_t *)kzalloc(257, GFP_KERNEL);
    if (zeroed == 0) return false;
    for (size_t i = 0; i < 257; ++i) {
        if (zeroed[i] != 0) {
            kfree(zeroed);
            return false;
        }
    }

    LIST_HEAD(nodes);
    struct compat_test_node *a = (struct compat_test_node *)kmalloc(sizeof(*a), GFP_KERNEL);
    struct compat_test_node *b = (struct compat_test_node *)kmalloc(sizeof(*b), GFP_ATOMIC);
    if (a == 0 || b == 0) {
        kfree(a);
        kfree(b);
        kfree(zeroed);
        return false;
    }

    a->value = 11;
    b->value = 31;
    INIT_LIST_HEAD(&a->link);
    INIT_LIST_HEAD(&b->link);
    list_add_tail(&a->link, &nodes);
    list_add_tail(&b->link, &nodes);

    uint32_t sum = 0;
    struct list_head *position;
    list_for_each(position, &nodes) {
        struct compat_test_node *node = list_entry(position, struct compat_test_node, link);
        sum += node->value;
    }

    const uint8_t array_probe[] = {1, 2, 3, 4};
    const bool basic_ok = sum == 42u && ARRAY_SIZE(array_probe) == 4u;

    list_del(&a->link);
    list_del(&b->link);
    kfree(a);
    kfree(b);
    kfree(zeroed);

    if (!basic_ok) return false;
    pr_info("compat self-test: slab/list/printk/types ready");

    if (!pci_init()) {
        pr_err("native PCI enumeration failed");
        return false;
    }

    pr_info("native PCI: %zu device(s) enumerated", pci_device_count());
    if (!linux_pci_compat_self_test()) {
        pr_err("Linux PCI driver bind/unbind self-test failed");
        return false;
    }

    pr_info("Linux PCI compatibility ready: config, BARs and driver matching; hardware driver binding deferred until IRQ bring-up");
    return true;
}
