#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <linux/types.h>

struct net_device;

struct sk_buff {
    u8 *head;
    u8 *data;
    u8 *tail;
    u8 *end;
    unsigned int len;
    struct net_device *dev;
    u16 protocol;
};

struct sk_buff *alloc_skb(unsigned int size, gfp_t flags);
struct sk_buff *dev_alloc_skb(unsigned int size);
struct sk_buff *netdev_alloc_skb(struct net_device *dev, unsigned int size);
void dev_kfree_skb(struct sk_buff *skb);
void dev_kfree_skb_any(struct sk_buff *skb);
void kfree_skb(struct sk_buff *skb);

u8 *skb_put(struct sk_buff *skb, unsigned int length);
void skb_reserve(struct sk_buff *skb, unsigned int length);
void skb_copy_to_linear_data(struct sk_buff *skb, const void *source, unsigned int length);
void skb_copy_to_linear_data_offset(struct sk_buff *skb,
                                    int offset,
                                    const void *source,
                                    unsigned int length);
void skb_copy_and_csum_dev(const struct sk_buff *skb, void *destination);
