#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <linux/device.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#define IFNAMSIZ 16

#define NETDEV_TX_OK   0
#define NETDEV_TX_BUSY 1

typedef int netdev_tx_t;

struct net_device;

struct net_device_stats {
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_bytes;
    u64 tx_bytes;
    u64 rx_errors;
    u64 tx_errors;
    u64 rx_dropped;
    u64 tx_dropped;
};

struct net_device_ops {
    int (*ndo_open)(struct net_device *dev);
    int (*ndo_stop)(struct net_device *dev);
    netdev_tx_t (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
};

struct net_device {
    struct device dev;
    char name[IFNAMSIZ];
    unsigned int irq;
    unsigned long base_addr;
    u8 dev_addr[ETH_ALEN];
    const struct net_device_ops *netdev_ops;
    struct net_device_stats stats;
    unsigned long trans_start;
    unsigned int mtu;
    bool registered;
    bool running;
    bool queue_stopped;
    void *priv;
};

struct net_device *alloc_etherdev(size_t private_size);
void free_netdev(struct net_device *dev);
void *netdev_priv(const struct net_device *dev);

int register_netdev(struct net_device *dev);
void unregister_netdev(struct net_device *dev);

void netif_start_queue(struct net_device *dev);
void netif_stop_queue(struct net_device *dev);
void netif_wake_queue(struct net_device *dev);
bool netif_queue_stopped(const struct net_device *dev);
bool netif_running(const struct net_device *dev);

int netif_rx(struct sk_buff *skb);
netdev_tx_t dev_queue_xmit(struct sk_buff *skb);

size_t linux_net_device_count(void);
struct net_device *linux_net_device_at(size_t index);
bool linux_net_run_arp_self_test(void);

#define SET_NETDEV_DEV(netdev, parent_dev) do { (void)(parent_dev); } while (0)

#define netdev_info(dev, format, ...) \
    printk("[linux:net] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_warn(dev, format, ...) \
    printk("[linux:net:warn] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_err(dev, format, ...) \
    printk("[linux:net:error] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_dbg(dev, format, ...) \
    printk("[linux:net:debug] %s: " format, (dev)->name, ##__VA_ARGS__)
