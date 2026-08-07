#include <stddef.h>
#include <stdint.h>

#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <twilight/linux_compat.h>

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

    const bool ok = sum == 42u && ARRAY_SIZE((uint8_t[]){1, 2, 3, 4}) == 4u;

    list_del(&a->link);
    list_del(&b->link);
    kfree(a);
    kfree(b);
    kfree(zeroed);

    if (ok) pr_info("compat self-test: slab/list/printk/types ready");
    return ok;
}
