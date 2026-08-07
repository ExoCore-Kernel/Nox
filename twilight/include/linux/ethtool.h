#pragma once

#include <linux/types.h>

#define ETH_GSTRING_LEN 32
#define ETH_SS_STATS 1

#define WAKE_PHY   (1u << 0)
#define WAKE_UCAST (1u << 1)
#define WAKE_MCAST (1u << 2)
#define WAKE_BCAST (1u << 3)
#define WAKE_MAGIC (1u << 5)

struct net_device;

struct ethtool_drvinfo {
    char driver[32];
    char version[32];
    char fw_version[32];
    char bus_info[32];
    u32 regdump_len;
};

struct ethtool_cmd {
    u32 supported;
    u32 advertising;
    u16 speed;
    u8 duplex;
    u8 port;
    u8 phy_address;
    u8 transceiver;
    u8 autoneg;
};

struct ethtool_regs {
    u32 cmd;
    u32 version;
    u32 len;
};

struct ethtool_wolinfo {
    u32 cmd;
    u32 supported;
    u32 wolopts;
};

struct ethtool_stats {
    u32 cmd;
    u32 n_stats;
};

struct ethtool_ops {
    void (*get_drvinfo)(struct net_device *, struct ethtool_drvinfo *);
    int (*get_settings)(struct net_device *, struct ethtool_cmd *);
    int (*set_settings)(struct net_device *, struct ethtool_cmd *);
    int (*get_regs_len)(struct net_device *);
    void (*get_regs)(struct net_device *, struct ethtool_regs *, void *);
    int (*nway_reset)(struct net_device *);
    u32 (*get_link)(struct net_device *);
    u32 (*get_msglevel)(struct net_device *);
    void (*set_msglevel)(struct net_device *, u32);
    void (*get_wol)(struct net_device *, struct ethtool_wolinfo *);
    int (*set_wol)(struct net_device *, struct ethtool_wolinfo *);
    void (*get_strings)(struct net_device *, u32, u8 *);
    int (*get_sset_count)(struct net_device *, int);
    void (*get_ethtool_stats)(struct net_device *, struct ethtool_stats *, u64 *);
};
