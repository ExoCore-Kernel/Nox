#include <stddef.h>
#include <stdint.h>

#include <linux/gfp.h>
#include <linux/skbuff.h>
#include <linux/slab.h>

static void bytes_copy(void *destination, const void *source, size_t size) {
    u8 *out = (u8 *)destination;
    const u8 *in = (const u8 *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

struct sk_buff *alloc_skb(unsigned int size, gfp_t flags) {
    struct sk_buff *skb = (struct sk_buff *)kzalloc(sizeof(*skb), flags);
    if (skb == 0) return 0;

    const unsigned int allocation_size = size == 0 ? 1u : size;
    skb->head = (u8 *)kmalloc(allocation_size, flags);
    if (skb->head == 0) {
        kfree(skb);
        return 0;
    }

    skb->data = skb->head;
    skb->tail = skb->head;
    skb->end = skb->head + allocation_size;
    skb->len = 0;
    skb->dev = 0;
    skb->protocol = 0;
    return skb;
}

struct sk_buff *dev_alloc_skb(unsigned int size) {
    return alloc_skb(size, GFP_ATOMIC);
}

struct sk_buff *netdev_alloc_skb(struct net_device *dev, unsigned int size) {
    struct sk_buff *skb = alloc_skb(size, GFP_ATOMIC);
    if (skb != 0) skb->dev = dev;
    return skb;
}

void dev_kfree_skb(struct sk_buff *skb) {
    if (skb == 0) return;
    kfree(skb->head);
    skb->head = 0;
    kfree(skb);
}

void kfree_skb(struct sk_buff *skb) {
    dev_kfree_skb(skb);
}

u8 *skb_put(struct sk_buff *skb, unsigned int length) {
    if (skb == 0 || skb->tail == 0 || skb->end == 0) return 0;
    if ((size_t)(skb->end - skb->tail) < (size_t)length) return 0;

    u8 *start = skb->tail;
    skb->tail += length;
    skb->len += length;
    return start;
}

void skb_reserve(struct sk_buff *skb, unsigned int length) {
    if (skb == 0 || skb->data == 0 || skb->tail == 0 || skb->end == 0) return;
    if ((size_t)(skb->end - skb->data) < (size_t)length) return;
    skb->data += length;
    skb->tail += length;
}

void skb_copy_and_csum_dev(const struct sk_buff *skb, void *destination) {
    if (skb == 0 || destination == 0 || skb->data == 0) return;
    bytes_copy(destination, skb->data, skb->len);
}
