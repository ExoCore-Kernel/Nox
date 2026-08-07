#pragma once

#include <stdbool.h>
#include <stddef.h>

#include <linux/device.h>
#include <linux/if_ether.h>
#include <linux/jiffies.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#define IFNAMSIZ 16

#define NETDEV_TX_OK   0
#define NETDEV_TX_BUSY 1

typedef int netdev_tx_t;
typedef u64 netdev_features_t;

#define NETIF_F_SG        (1ull << 0)
#define NETIF_F_HW_CSUM   (1ull << 1)
#define NETIF_F_HIGHDMA   (1ull << 2)
#define NETIF_F_RXALL     (1ull << 3)
#define NETIF_F_RXFCS     (1ull << 4)

#define NETIF_MSG_DRV       (1u << 0)
#define NETIF_MSG_PROBE     (1u << 1)
#define NETIF_MSG_LINK      (1u << 2)
#define NETIF_MSG_IFUP      (1u << 3)
#define NETIF_MSG_IFDOWN    (1u << 4)
#define NETIF_MSG_TX_QUEUED (1u << 5)
#define NETIF_MSG_TX_ERR    (1u << 6)
#define NETIF_MSG_RX_ERR    (1u << 7)
#define NETIF_MSG_RX_STATUS (1u << 8)
#define NETIF_MSG_INTR      (1u << 9)

#define IFF_PROMISC  0x0100u
#define IFF_ALLMULTI 0x0200u

struct ethtool_ops;
struct ifreq;
struct net_device;

/* Linux 2.6 multicast list representation used by the unmodified 8139too
 * source. Twilight currently leaves the list empty unless a networking layer
 * explicitly installs multicast addresses. */
struct dev_mc_list {
    struct dev_mc_list *next;
    u8 dmi_addr[ETH_ALEN];
};

struct net_device_stats {
    u64 rx_packets;
    u64 tx_packets;
    u64 rx_bytes;
    u64 tx_bytes;
    u64 rx_errors;
    u64 tx_errors;
    u64 rx_dropped;
    u64 tx_dropped;
    u64 multicast;
    u64 collisions;
    u64 rx_length_errors;
    u64 rx_over_errors;
    u64 rx_crc_errors;
    u64 rx_frame_errors;
    u64 rx_fifo_errors;
    u64 rx_missed_errors;
    u64 tx_aborted_errors;
    u64 tx_carrier_errors;
    u64 tx_fifo_errors;
    u64 tx_heartbeat_errors;
    u64 tx_window_errors;
};

struct napi_struct {
    struct net_device *dev;
    int (*poll)(struct napi_struct *napi, int budget);
    int weight;
    bool enabled;
    bool scheduled;
    struct napi_struct *next;
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
    u8 perm_addr[ETH_ALEN];
    unsigned int addr_len;

    const struct net_device_ops *netdev_ops;
    const struct ethtool_ops *ethtool_ops;

    /* Linux 2.6-era net_device entry points used by the pinned upstream
     * 8139too source. The compatibility core accepts either these or
     * netdev_ops so newer Twilight drivers keep working unchanged. */
    int (*open)(struct net_device *dev);
    int (*stop)(struct net_device *dev);
    netdev_tx_t (*hard_start_xmit)(struct sk_buff *skb, struct net_device *dev);
    struct net_device_stats *(*get_stats)(struct net_device *dev);
    void (*set_multicast_list)(struct net_device *dev);
    int (*do_ioctl)(struct net_device *dev, struct ifreq *rq, int cmd);
    void (*tx_timeout)(struct net_device *dev);

    struct net_device_stats stats;
    unsigned long trans_start;
    unsigned long watchdog_timeo;
    unsigned int mtu;
    unsigned int flags;
    netdev_features_t features;
    netdev_features_t vlan_features;
    netdev_features_t hw_features;
    unsigned int min_mtu;
    unsigned int max_mtu;

    struct dev_mc_list *mc_list;
    int mc_count;

    bool registered;
    bool running;
    bool queue_stopped;
    bool device_attached;
    bool carrier;
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

static inline void netif_carrier_on(struct net_device *dev) {
    if (dev != 0) dev->carrier = true;
}
static inline void netif_carrier_off(struct net_device *dev) {
    if (dev != 0) dev->carrier = false;
}
static inline bool netif_carrier_ok(const struct net_device *dev) {
    return dev != 0 && dev->carrier;
}

void netif_napi_add(struct net_device *dev,
                    struct napi_struct *napi,
                    int (*poll)(struct napi_struct *, int),
                    int weight);
void napi_enable(struct napi_struct *napi);
void napi_disable(struct napi_struct *napi);
bool netif_rx_schedule_prep(struct net_device *dev, struct napi_struct *napi);
void __netif_rx_schedule(struct net_device *dev, struct napi_struct *napi);
void __netif_rx_complete(struct net_device *dev, struct napi_struct *napi);
void linux_napi_poll(void);

int netif_rx(struct sk_buff *skb);
int netif_receive_skb(struct sk_buff *skb);
netdev_tx_t dev_queue_xmit(struct sk_buff *skb);

void netif_device_detach(struct net_device *dev);
void netif_device_attach(struct net_device *dev);

size_t linux_net_device_count(void);
struct net_device *linux_net_device_at(size_t index);
bool linux_net_run_arp_self_test(void);

#define SET_NETDEV_DEV(netdev, parent_dev) do { (void)(parent_dev); } while (0)
#define SET_MODULE_OWNER(netdev) do { (void)(netdev); } while (0)

#define netif_msg_drv(tp)       (((tp)->msg_enable & NETIF_MSG_DRV) != 0)
#define netif_msg_probe(tp)     (((tp)->msg_enable & NETIF_MSG_PROBE) != 0)
#define netif_msg_link(tp)      (((tp)->msg_enable & NETIF_MSG_LINK) != 0)
#define netif_msg_ifup(tp)      (((tp)->msg_enable & NETIF_MSG_IFUP) != 0)
#define netif_msg_ifdown(tp)    (((tp)->msg_enable & NETIF_MSG_IFDOWN) != 0)
#define netif_msg_tx_queued(tp) (((tp)->msg_enable & NETIF_MSG_TX_QUEUED) != 0)
#define netif_msg_tx_err(tp)    (((tp)->msg_enable & NETIF_MSG_TX_ERR) != 0)
#define netif_msg_rx_err(tp)    (((tp)->msg_enable & NETIF_MSG_RX_ERR) != 0)

#define netdev_info(dev, format, ...) \
    printk("[linux:net] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_warn(dev, format, ...) \
    printk("[linux:net:warn] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_err(dev, format, ...) \
    printk("[linux:net:error] %s: " format, (dev)->name, ##__VA_ARGS__)
#define netdev_dbg(dev, format, ...) \
    printk("[linux:net:debug] %s: " format, (dev)->name, ##__VA_ARGS__)

#define netif_dbg(priv, type, dev, format, ...) do { \
    (void)(priv); \
    netdev_dbg((dev), format, ##__VA_ARGS__); \
} while (0)
