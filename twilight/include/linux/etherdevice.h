#pragma once

#include <stdbool.h>

#include <linux/if_ether.h>
#include <linux/netdevice.h>

static inline void ether_addr_copy(u8 *destination, const u8 *source) {
    for (unsigned int i = 0; i < ETH_ALEN; ++i) destination[i] = source[i];
}

static inline void eth_hw_addr_set(struct net_device *dev, const u8 *address) {
    if (dev == 0 || address == 0) return;
    ether_addr_copy(dev->dev_addr, address);
}

static inline bool is_zero_ether_addr(const u8 *address) {
    if (address == 0) return true;
    u8 value = 0;
    for (unsigned int i = 0; i < ETH_ALEN; ++i) value |= address[i];
    return value == 0;
}

static inline bool is_multicast_ether_addr(const u8 *address) {
    return address != 0 && (address[0] & 1u) != 0;
}

static inline bool is_valid_ether_addr(const u8 *address) {
    return address != 0 && !is_zero_ether_addr(address) && !is_multicast_ether_addr(address);
}

static inline int eth_validate_addr(struct net_device *dev) {
    return dev != 0 && is_valid_ether_addr(dev->dev_addr) ? 0 : -1;
}

u16 eth_type_trans(struct sk_buff *skb, struct net_device *dev);
