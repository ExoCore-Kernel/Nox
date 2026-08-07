#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/netdevice.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <twilight/timer.h>

#define LINUX_NET_MAX_DEVICES 8u

static struct net_device *registered_devices[LINUX_NET_MAX_DEVICES];
static size_t registered_count;

static volatile bool arp_test_pending;
static volatile bool arp_reply_seen;
static struct net_device *arp_test_device;

static void bytes_zero(void *pointer, size_t size) {
    u8 *bytes = (u8 *)pointer;
    for (size_t i = 0; i < size; ++i) bytes[i] = 0;
}

static bool bytes_equal(const u8 *a, const u8 *b, size_t size) {
    if (a == 0 || b == 0) return false;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

static void copy_bytes(u8 *destination, const u8 *source, size_t size) {
    for (size_t i = 0; i < size; ++i) destination[i] = source[i];
}

struct net_device *alloc_etherdev(size_t private_size) {
    const size_t alignment = 16u;
    if (private_size > (size_t)-1 - sizeof(struct net_device) - alignment) return 0;

    const size_t total = sizeof(struct net_device) + alignment + private_size;
    struct net_device *dev = (struct net_device *)kzalloc(total, GFP_KERNEL);
    if (dev == 0) return 0;

    uintptr_t private_address = (uintptr_t)(dev + 1);
    private_address = (private_address + alignment - 1u) & ~(uintptr_t)(alignment - 1u);

    dev->priv = (void *)private_address;
    dev->mtu = ETH_DATA_LEN;
    dev->registered = false;
    dev->running = false;
    dev->queue_stopped = true;
    return dev;
}

void free_netdev(struct net_device *dev) {
    if (dev == 0) return;
    kfree(dev);
}

void *netdev_priv(const struct net_device *dev) {
    return dev != 0 ? dev->priv : 0;
}

static void assign_eth_name(struct net_device *dev, size_t index) {
    bytes_zero(dev->name, sizeof(dev->name));
    dev->name[0] = 'e';
    dev->name[1] = 't';
    dev->name[2] = 'h';
    if (index < 10u) {
        dev->name[3] = (char)('0' + index);
        dev->name[4] = '\0';
    } else {
        dev->name[3] = (char)('0' + ((index / 10u) % 10u));
        dev->name[4] = (char)('0' + (index % 10u));
        dev->name[5] = '\0';
    }
    dev->dev.init_name = dev->name;
}

int register_netdev(struct net_device *dev) {
    if (dev == 0 || dev->registered || dev->netdev_ops == 0 ||
        dev->netdev_ops->ndo_start_xmit == 0) {
        return -1;
    }
    if (registered_count >= LINUX_NET_MAX_DEVICES) return -1;
    if (!is_valid_ether_addr(dev->dev_addr)) return -1;

    assign_eth_name(dev, registered_count);
    registered_devices[registered_count++] = dev;
    dev->registered = true;

    netdev_info(dev,
                "registered Ethernet device MAC %02x:%02x:%02x:%02x:%02x:%02x IRQ %u",
                dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
                dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5], dev->irq);
    return 0;
}

void unregister_netdev(struct net_device *dev) {
    if (dev == 0 || !dev->registered) return;

    if (dev->running && dev->netdev_ops != 0 && dev->netdev_ops->ndo_stop != 0) {
        (void)dev->netdev_ops->ndo_stop(dev);
    }

    for (size_t i = 0; i < registered_count; ++i) {
        if (registered_devices[i] != dev) continue;
        for (size_t j = i + 1u; j < registered_count; ++j)
            registered_devices[j - 1u] = registered_devices[j];
        --registered_count;
        registered_devices[registered_count] = 0;
        break;
    }

    dev->registered = false;
    dev->running = false;
    dev->queue_stopped = true;
}

void netif_start_queue(struct net_device *dev) {
    if (dev != 0) dev->queue_stopped = false;
}

void netif_stop_queue(struct net_device *dev) {
    if (dev != 0) dev->queue_stopped = true;
}

void netif_wake_queue(struct net_device *dev) {
    if (dev != 0) dev->queue_stopped = false;
}

bool netif_queue_stopped(const struct net_device *dev) {
    return dev == 0 || dev->queue_stopped;
}

bool netif_running(const struct net_device *dev) {
    return dev != 0 && dev->running;
}

u16 eth_type_trans(struct sk_buff *skb, struct net_device *dev) {
    if (skb == 0 || skb->data == 0 || skb->len < ETH_HLEN) return 0;
    skb->dev = dev;
    skb->protocol = (u16)(((u16)skb->data[12] << 8) | (u16)skb->data[13]);
    return skb->protocol;
}

static void inspect_arp_reply(const struct sk_buff *skb) {
    if (!arp_test_pending || arp_test_device == 0 || skb == 0 || skb->data == 0)
        return;
    if (skb->dev != arp_test_device || skb->len < 42u) return;

    const u8 *frame = skb->data;
    if (frame[12] != 0x08u || frame[13] != 0x06u) return;

    const u8 *arp = frame + ETH_HLEN;
    if (arp[6] != 0x00u || arp[7] != 0x02u) return; /* ARP reply */

    static const u8 gateway_ip[4] = {10u, 0u, 2u, 2u};
    static const u8 guest_ip[4] = {10u, 0u, 2u, 15u};
    if (!bytes_equal(arp + 14u, gateway_ip, 4u)) return;
    if (!bytes_equal(arp + 24u, guest_ip, 4u)) return;
    if (!bytes_equal(arp + 18u, arp_test_device->dev_addr, ETH_ALEN)) return;

    arp_reply_seen = true;
}

int netif_rx(struct sk_buff *skb) {
    if (skb == 0) return -1;

    if (skb->dev != 0) {
        ++skb->dev->stats.rx_packets;
        skb->dev->stats.rx_bytes += skb->len;
    }

    inspect_arp_reply(skb);
    dev_kfree_skb(skb);
    return 0;
}

netdev_tx_t dev_queue_xmit(struct sk_buff *skb) {
    if (skb == 0 || skb->dev == 0 || skb->dev->netdev_ops == 0 ||
        skb->dev->netdev_ops->ndo_start_xmit == 0) {
        if (skb != 0) dev_kfree_skb(skb);
        return NETDEV_TX_BUSY;
    }

    struct net_device *dev = skb->dev;
    if (!dev->running || dev->queue_stopped) {
        dev_kfree_skb(skb);
        return NETDEV_TX_BUSY;
    }

    return dev->netdev_ops->ndo_start_xmit(skb, dev);
}

size_t linux_net_device_count(void) {
    return registered_count;
}

struct net_device *linux_net_device_at(size_t index) {
    return index < registered_count ? registered_devices[index] : 0;
}

static bool bring_device_up(struct net_device *dev) {
    if (dev == 0 || !dev->registered || dev->netdev_ops == 0) return false;
    if (dev->running) return true;
    if (dev->netdev_ops->ndo_open == 0) return false;

    const int result = dev->netdev_ops->ndo_open(dev);
    if (result != 0) {
        netdev_err(dev, "ndo_open failed: %d", result);
        return false;
    }

    dev->running = true;
    return true;
}

static struct sk_buff *build_qemu_gateway_arp(struct net_device *dev) {
    struct sk_buff *skb = alloc_skb(64u, GFP_KERNEL);
    if (skb == 0) return 0;

    u8 *frame = skb_put(skb, 42u);
    if (frame == 0) {
        dev_kfree_skb(skb);
        return 0;
    }

    for (unsigned int i = 0; i < ETH_ALEN; ++i) frame[i] = 0xffu;
    copy_bytes(frame + 6u, dev->dev_addr, ETH_ALEN);
    frame[12] = 0x08u;
    frame[13] = 0x06u;

    u8 *arp = frame + ETH_HLEN;
    arp[0] = 0x00u; arp[1] = 0x01u; /* Ethernet */
    arp[2] = 0x08u; arp[3] = 0x00u; /* IPv4 */
    arp[4] = ETH_ALEN;
    arp[5] = 4u;
    arp[6] = 0x00u; arp[7] = 0x01u; /* request */
    copy_bytes(arp + 8u, dev->dev_addr, ETH_ALEN);
    arp[14] = 10u; arp[15] = 0u; arp[16] = 2u; arp[17] = 15u;
    for (unsigned int i = 0; i < ETH_ALEN; ++i) arp[18u + i] = 0u;
    arp[24] = 10u; arp[25] = 0u; arp[26] = 2u; arp[27] = 2u;

    skb->dev = dev;
    skb->protocol = ETH_P_ARP;
    return skb;
}

bool linux_net_run_arp_self_test(void) {
    if (registered_count == 0) return true;

    struct net_device *dev = registered_devices[0];
    if (!bring_device_up(dev)) return false;

    struct sk_buff *skb = build_qemu_gateway_arp(dev);
    if (skb == 0) {
        netdev_err(dev, "unable to allocate ARP self-test packet");
        return false;
    }

    arp_test_device = dev;
    arp_reply_seen = false;
    arp_test_pending = true;

    netdev_info(dev, "sending ARP request for QEMU user-network gateway 10.0.2.2");
    if (dev_queue_xmit(skb) != NETDEV_TX_OK) {
        arp_test_pending = false;
        netdev_err(dev, "ARP self-test transmit queue rejected packet");
        return false;
    }

    for (unsigned int elapsed = 0; elapsed < 1000u && !arp_reply_seen; elapsed += 10u)
        timer_sleep_ms(10u);

    arp_test_pending = false;

    if (!arp_reply_seen) {
        netdev_warn(dev,
                    "ARP self-test timed out after 1000 ms (TX packets=%llu RX packets=%llu)",
                    (unsigned long long)dev->stats.tx_packets,
                    (unsigned long long)dev->stats.rx_packets);
        return false;
    }

    netdev_info(dev,
                "ARP self-test PASS: received gateway reply; Ethernet TX DMA + IRQ + RX DMA path works");
    return true;
}
