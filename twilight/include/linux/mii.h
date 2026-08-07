#pragma once

#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/types.h>

#define MII_BMCR 0
#define MII_BMSR 1
#define MII_ADVERTISE 4
#define MII_LPA 5
#define BMCR_ANRESTART 0x0200
#define BMSR_LSTATUS 0x0004
#define LPA_100FULL 0x0100

struct net_device;

struct mii_if_info {
    struct net_device *dev;
    int phy_id;
    unsigned int phy_id_mask;
    unsigned int reg_num_mask;
    int full_duplex;
    int force_media;
    int supports_gmii;
    int (*mdio_read)(struct net_device *dev, int phy_id, int location);
    void (*mdio_write)(struct net_device *dev, int phy_id, int location, int value);
};

struct mii_ioctl_data {
    u16 phy_id;
    u16 reg_num;
    u16 val_in;
    u16 val_out;
};

struct ifreq { unsigned char ifr_ifru[32]; };
#define if_mii(rq) ((struct mii_ioctl_data *)((rq)->ifr_ifru))

static inline int mii_ethtool_gset(struct mii_if_info *mii, struct ethtool_cmd *cmd) {
    if (mii == 0 || cmd == 0) return -EINVAL;
    cmd->duplex = mii->full_duplex ? 1u : 0u;
    cmd->phy_address = (u8)mii->phy_id;
    return 0;
}

static inline int mii_ethtool_sset(struct mii_if_info *mii, struct ethtool_cmd *cmd) {
    if (mii == 0 || cmd == 0) return -EINVAL;
    mii->full_duplex = cmd->duplex != 0;
    return 0;
}

static inline int mii_nway_restart(struct mii_if_info *mii) {
    if (mii == 0 || mii->mdio_read == 0 || mii->mdio_write == 0) return -EINVAL;
    const int control = mii->mdio_read(mii->dev, mii->phy_id, MII_BMCR);
    mii->mdio_write(mii->dev, mii->phy_id, MII_BMCR, control | BMCR_ANRESTART);
    return 0;
}

static inline int mii_link_ok(struct mii_if_info *mii) {
    if (mii == 0 || mii->mdio_read == 0) return 0;
    return (mii->mdio_read(mii->dev, mii->phy_id, MII_BMSR) & BMSR_LSTATUS) != 0;
}

static inline unsigned int mii_check_media(struct mii_if_info *mii,
                                           unsigned int ok_to_print,
                                           unsigned int init_media) {
    (void)ok_to_print;
    (void)init_media;
    if (mii == 0 || mii->mdio_read == 0) return 0;
    const int partner = mii->mdio_read(mii->dev, mii->phy_id, MII_LPA);
    if (!mii->force_media && partner != 0xffff)
        mii->full_duplex = (partner & LPA_100FULL) != 0;
    return (unsigned int)mii_link_ok(mii);
}

static inline int generic_mii_ioctl(struct mii_if_info *mii,
                                    struct mii_ioctl_data *data,
                                    int cmd,
                                    unsigned int *duplex_changed) {
    (void)mii;
    (void)data;
    (void)cmd;
    if (duplex_changed != 0) *duplex_changed = 0;
    return -ENOSYS;
}
