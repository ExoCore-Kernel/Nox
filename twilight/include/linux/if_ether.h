#pragma once

#include <linux/types.h>

#define ETH_ALEN 6
#define ETH_HLEN 14
#define ETH_ZLEN 60
#define ETH_DATA_LEN 1500
#define ETH_FRAME_LEN 1514
#define ETH_FCS_LEN 4

#define ETH_P_IP  0x0800u
#define ETH_P_ARP 0x0806u

struct ethhdr {
    u8 h_dest[ETH_ALEN];
    u8 h_source[ETH_ALEN];
    u16 h_proto;
} __attribute__((packed));
