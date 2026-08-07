/*
 * RTL8139 Ethernet driver for Twilight's Linux compatibility layer.
 *
 * Hardware flow and register programming are derived from Linux 8139too.c,
 * originally maintained by Jeff Garzik and derived in part from Donald
 * Becker's rtl8139 driver. This compatibility port intentionally keeps the
 * classic Linux 8139too DMA/ring/interrupt model while omitting ethtool, MII
 * tuning and power-management paths that are not required for first network
 * bring-up.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#define DRV_NAME "8139too"

#define RTL8139_VENDOR_ID 0x10ecu
#define RTL8139_DEVICE_ID 0x8139u

#define RTL_MIN_IO_SIZE 0x80u
#define NUM_TX_DESC 4u
#define TX_BUF_SIZE 1792u
#define TX_BUF_TOT_LEN (TX_BUF_SIZE * NUM_TX_DESC)
#define RX_BUF_LEN (8192u << 2)
#define RX_BUF_PAD 16u
#define RX_BUF_WRAP_PAD 2048u
#define RX_BUF_TOT_LEN (RX_BUF_LEN + RX_BUF_PAD + RX_BUF_WRAP_PAD)
#define TX_FIFO_THRESH 256u

/* RTL8139 register offsets used by Linux's classic 8139too data path. */
enum rtl8139_registers {
    MAC0       = 0x00,
    TxStatus0  = 0x10,
    TxAddr0    = 0x20,
    RxBuf      = 0x30,
    ChipCmd    = 0x37,
    RxBufPtr   = 0x38,
    IntrMask   = 0x3c,
    IntrStatus = 0x3e,
    TxConfig   = 0x40,
    RxConfig   = 0x44,
    RxMissed   = 0x4c,
    Cfg9346    = 0x50,
    MultiIntr  = 0x5c,
};

enum rtl8139_command_bits {
    CmdReset = 0x10,
    CmdRxEnb = 0x08,
    CmdTxEnb = 0x04,
    RxBufEmpty = 0x01,
};

enum rtl8139_interrupt_bits {
    PCIErr     = 0x8000,
    PCSTimeout = 0x4000,
    RxFIFOOver = 0x0040,
    RxUnderrun = 0x0020,
    RxOverflow = 0x0010,
    TxErr      = 0x0008,
    TxOK       = 0x0004,
    RxErr      = 0x0002,
    RxOK       = 0x0001,
};

enum rtl8139_tx_status_bits {
    TxUnderrun    = 0x00004000,
    TxStatOK      = 0x00008000,
    TxOutOfWindow = 0x20000000,
    TxAborted     = 0x40000000,
};

enum rtl8139_rx_status_bits {
    RxStatusOK = 0x0001,
    RxBadAlign = 0x0002,
    RxCRCErr   = 0x0004,
    RxTooLong  = 0x0008,
    RxRunt     = 0x0010,
    RxBadSymbol = 0x0020,
};

enum rtl8139_rx_mode_bits {
    AcceptBroadcast = 0x08,
    AcceptMyPhys    = 0x02,
};

#define RxCfgFIFOShift 13u
#define RxCfgDMAShift 8u
#define RxCfgRcv32K (1u << 12)
#define RxNoWrap (1u << 7)
#define RX_FIFO_THRESH 7u
#define RX_DMA_BURST 7u
#define TX_DMA_BURST 6u
#define TX_RETRY 8u
#define TxIFGShift 24u
#define TxIFG96 (3u << TxIFGShift)
#define TxDMAShift 8u
#define TxRetryShift 4u

#define Cfg9346_Lock 0x00u
#define Cfg9346_Unlock 0xc0u
#define MultiIntrClear 0xf000u

static const u16 rtl8139_intr_mask =
    PCIErr | PCSTimeout | RxUnderrun | RxOverflow | RxFIFOOver |
    TxErr | TxOK | RxErr | RxOK;

static const u32 rtl8139_rx_config =
    RxCfgRcv32K | RxNoWrap |
    (RX_FIFO_THRESH << RxCfgFIFOShift) |
    (RX_DMA_BURST << RxCfgDMAShift) |
    AcceptBroadcast | AcceptMyPhys;

static const u32 rtl8139_tx_config =
    TxIFG96 | (TX_DMA_BURST << TxDMAShift) | (TX_RETRY << TxRetryShift);

struct rtl8139_private {
    struct pci_dev *pdev;
    struct net_device *dev;
    void __iomem *ioaddr;
    int mmio_bar;

    u8 *rx_ring;
    dma_addr_t rx_ring_dma;
    unsigned int cur_rx;

    u8 *tx_bufs;
    dma_addr_t tx_bufs_dma;
    u8 *tx_buf[NUM_TX_DESC];
    unsigned int tx_len[NUM_TX_DESC];
    unsigned int cur_tx;
    unsigned int dirty_tx;
    u32 tx_flag;

    spinlock_t lock;
    bool irq_registered;
    bool dma_ready;
    bool first_rx_logged;
    bool first_tx_logged;
};

static inline u8 rtl_r8(struct rtl8139_private *tp, unsigned int reg) {
    return readb((u8 __iomem *)tp->ioaddr + reg);
}

static inline u16 rtl_r16(struct rtl8139_private *tp, unsigned int reg) {
    return readw((u8 __iomem *)tp->ioaddr + reg);
}

static inline u32 rtl_r32(struct rtl8139_private *tp, unsigned int reg) {
    return readl((u8 __iomem *)tp->ioaddr + reg);
}

static inline void rtl_w8(struct rtl8139_private *tp, unsigned int reg, u8 value) {
    writeb(value, (u8 __iomem *)tp->ioaddr + reg);
}

static inline void rtl_w16(struct rtl8139_private *tp, unsigned int reg, u16 value) {
    writew(value, (u8 __iomem *)tp->ioaddr + reg);
}

static inline void rtl_w32(struct rtl8139_private *tp, unsigned int reg, u32 value) {
    writel(value, (u8 __iomem *)tp->ioaddr + reg);
}

static inline void rtl_w16_flush(struct rtl8139_private *tp, unsigned int reg, u16 value) {
    rtl_w16(tp, reg, value);
    (void)rtl_r16(tp, reg);
}

static inline void rtl_w32_flush(struct rtl8139_private *tp, unsigned int reg, u32 value) {
    rtl_w32(tp, reg, value);
    (void)rtl_r32(tp, reg);
}

static void bytes_copy(void *destination, const void *source, size_t size) {
    u8 *out = (u8 *)destination;
    const u8 *in = (const u8 *)source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
}

static void bytes_zero(void *destination, size_t size) {
    u8 *out = (u8 *)destination;
    for (size_t i = 0; i < size; ++i) out[i] = 0;
}

static bool rtl8139_chip_reset(struct rtl8139_private *tp) {
    rtl_w8(tp, ChipCmd, CmdReset);
    for (unsigned int i = 0; i < 1000u; ++i) {
        barrier();
        if ((rtl_r8(tp, ChipCmd) & CmdReset) == 0) return true;
        udelay(10u);
    }
    return false;
}

static void rtl8139_init_ring(struct rtl8139_private *tp) {
    tp->cur_rx = 0;
    tp->cur_tx = 0;
    tp->dirty_tx = 0;

    for (unsigned int i = 0; i < NUM_TX_DESC; ++i) {
        tp->tx_buf[i] = tp->tx_bufs + i * TX_BUF_SIZE;
        tp->tx_len[i] = 0;
    }
}

static void rtl8139_write_mac(struct rtl8139_private *tp) {
    rtl_w8(tp, Cfg9346, Cfg9346_Unlock);
    for (unsigned int i = 0; i < ETH_ALEN; ++i)
        rtl_w8(tp, MAC0 + i, tp->dev->dev_addr[i]);
    rtl_w8(tp, Cfg9346, Cfg9346_Lock);
}

static bool rtl8139_hw_start(struct rtl8139_private *tp) {
    if (!rtl8139_chip_reset(tp)) return false;

    rtl8139_write_mac(tp);

    /* Linux 8139too enables RX/TX before programming thresholds. */
    rtl_w8(tp, ChipCmd, CmdRxEnb | CmdTxEnb);
    rtl_w32(tp, RxConfig, rtl8139_rx_config);
    rtl_w32(tp, TxConfig, rtl8139_tx_config);

    tp->cur_rx = 0;
    rtl_w32_flush(tp, RxBuf, (u32)tp->rx_ring_dma);

    for (unsigned int i = 0; i < NUM_TX_DESC; ++i) {
        const dma_addr_t address = tp->tx_bufs_dma + (dma_addr_t)(i * TX_BUF_SIZE);
        rtl_w32_flush(tp, TxAddr0 + i * 4u, (u32)address);
    }

    rtl_w32(tp, RxMissed, 0u);
    rtl_w16(tp, MultiIntr, (u16)(rtl_r16(tp, MultiIntr) & (u16)~MultiIntrClear));

    /* Clear stale causes before exposing the device interrupt line. */
    rtl_w16(tp, IntrStatus, 0xffffu);
    wmb();
    rtl_w16(tp, IntrMask, rtl8139_intr_mask);
    (void)rtl_r16(tp, IntrMask);

    return (rtl_r8(tp, ChipCmd) & (CmdRxEnb | CmdTxEnb)) ==
           (CmdRxEnb | CmdTxEnb);
}

static void rtl8139_tx_complete(struct rtl8139_private *tp) {
    struct net_device *dev = tp->dev;

    while (tp->dirty_tx != tp->cur_tx) {
        const unsigned int entry = tp->dirty_tx % NUM_TX_DESC;
        const u32 status = rtl_r32(tp, TxStatus0 + entry * 4u);

        if ((status & (TxStatOK | TxUnderrun | TxAborted)) == 0) break;

        if ((status & (TxOutOfWindow | TxAborted)) != 0) {
            ++dev->stats.tx_errors;
            netdev_warn(dev, "RTL8139 transmit error status=0x%08x", status);
        } else {
            ++dev->stats.tx_packets;
            dev->stats.tx_bytes += tp->tx_len[entry];
            if (!tp->first_tx_logged) {
                tp->first_tx_logged = true;
                netdev_info(dev, "first Ethernet frame completed by RTL8139 TX DMA");
            }
        }

        tp->tx_len[entry] = 0;
        ++tp->dirty_tx;
    }

    if (netif_queue_stopped(dev) && tp->cur_tx - tp->dirty_tx < NUM_TX_DESC)
        netif_wake_queue(dev);
}

static void rtl8139_rx(struct rtl8139_private *tp) {
    struct net_device *dev = tp->dev;

    for (unsigned int budget = 0; budget < 64u; ++budget) {
        if ((rtl_r8(tp, ChipCmd) & RxBufEmpty) != 0) break;

        const unsigned int ring_offset = tp->cur_rx % RX_BUF_LEN;
        volatile u32 *header = (volatile u32 *)(void *)(tp->rx_ring + ring_offset);
        rmb();
        const u32 rx_status = *header;
        const u16 status = (u16)(rx_status & 0xffffu);
        const unsigned int rx_size = (unsigned int)(rx_status >> 16);

        if (rx_size < 8u || rx_size > 2048u ||
            (status & RxStatusOK) == 0 ||
            (status & (RxBadAlign | RxCRCErr | RxTooLong | RxRunt | RxBadSymbol)) != 0) {
            ++dev->stats.rx_errors;
            netdev_warn(dev,
                        "RTL8139 receive error status=0x%04x size=%u; resetting RX cursor",
                        status, rx_size);
            tp->cur_rx = 0;
            rtl_w16(tp, RxBufPtr, 0u);
            break;
        }

        const unsigned int packet_size = rx_size - ETH_FCS_LEN;
        struct sk_buff *skb = netdev_alloc_skb(dev, packet_size + 2u);
        if (skb == 0) {
            ++dev->stats.rx_dropped;
        } else {
            skb_reserve(skb, 2u);
            u8 *packet = skb_put(skb, packet_size);
            if (packet == 0) {
                ++dev->stats.rx_dropped;
                dev_kfree_skb(skb);
            } else {
                /* RxNoWrap plus RX_BUF_WRAP_PAD gives us a contiguous packet. */
                bytes_copy(packet, tp->rx_ring + ring_offset + 4u, packet_size);
                (void)eth_type_trans(skb, dev);
                if (!tp->first_rx_logged) {
                    tp->first_rx_logged = true;
                    netdev_info(dev,
                                "first Ethernet frame received through RTL8139 RX DMA (%u bytes, ethertype=0x%04x)",
                                packet_size, skb->protocol);
                }
                (void)netif_rx(skb);
            }
        }

        tp->cur_rx = (tp->cur_rx + rx_size + 4u + 3u) & ~3u;
        rtl_w16(tp, RxBufPtr, (u16)(tp->cur_rx - 16u));
    }
}

static irqreturn_t rtl8139_interrupt(int irq, void *dev_id) {
    struct net_device *dev = (struct net_device *)dev_id;
    if (dev == 0 || (unsigned int)irq != dev->irq) return IRQ_NONE;

    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    if (tp == 0 || tp->ioaddr == 0) return IRQ_NONE;

    bool handled = false;

    for (unsigned int loop = 0; loop < 20u; ++loop) {
        const u16 status = rtl_r16(tp, IntrStatus);
        if ((status & rtl8139_intr_mask) == 0) break;

        handled = true;
        rtl_w16(tp, IntrStatus, status);

        if ((status & (RxOK | RxErr | RxOverflow | RxFIFOOver)) != 0)
            rtl8139_rx(tp);

        if ((status & (TxOK | TxErr)) != 0) {
            unsigned long flags;
            spin_lock_irqsave(&tp->lock, flags);
            rtl8139_tx_complete(tp);
            spin_unlock_irqrestore(&tp->lock, flags);
        }

        if ((status & (PCIErr | PCSTimeout)) != 0) {
            netdev_warn(dev, "RTL8139 PCI error interrupt status=0x%04x", status);
        }
    }

    return handled ? IRQ_HANDLED : IRQ_NONE;
}

static int rtl8139_open(struct net_device *dev) {
    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    if (tp == 0 || tp->pdev == 0 || tp->ioaddr == 0) return -1;

    /* Keep the device quiet until the shared handler and DMA buffers exist. */
    rtl_w16(tp, IntrMask, 0u);
    rtl_w16(tp, IntrStatus, 0xffffu);

    if (request_irq(dev->irq, rtl8139_interrupt, IRQF_SHARED, dev->name, dev) != 0) {
        netdev_err(dev, "unable to register shared IRQ %u", dev->irq);
        return -1;
    }
    tp->irq_registered = true;

    tp->tx_bufs = (u8 *)dma_alloc_coherent(&tp->pdev->dev,
                                            TX_BUF_TOT_LEN,
                                            &tp->tx_bufs_dma,
                                            GFP_KERNEL);
    tp->rx_ring = (u8 *)dma_alloc_coherent(&tp->pdev->dev,
                                            RX_BUF_TOT_LEN,
                                            &tp->rx_ring_dma,
                                            GFP_KERNEL);

    if (tp->tx_bufs == 0 || tp->rx_ring == 0) {
        netdev_err(dev, "unable to allocate RTL8139 coherent DMA rings");
        if (tp->tx_bufs != 0)
            dma_free_coherent(&tp->pdev->dev, TX_BUF_TOT_LEN,
                              tp->tx_bufs, tp->tx_bufs_dma);
        if (tp->rx_ring != 0)
            dma_free_coherent(&tp->pdev->dev, RX_BUF_TOT_LEN,
                              tp->rx_ring, tp->rx_ring_dma);
        tp->tx_bufs = 0;
        tp->rx_ring = 0;
        free_irq(dev->irq, dev);
        tp->irq_registered = false;
        return -1;
    }

    tp->dma_ready = true;
    tp->tx_flag = (TX_FIFO_THRESH << 11) & 0x003f0000u;
    bytes_zero(tp->rx_ring, RX_BUF_TOT_LEN);
    bytes_zero(tp->tx_bufs, TX_BUF_TOT_LEN);
    rtl8139_init_ring(tp);

    if (!rtl8139_hw_start(tp)) {
        netdev_err(dev, "RTL8139 failed hardware reset/start sequence");
        dma_free_coherent(&tp->pdev->dev, TX_BUF_TOT_LEN,
                          tp->tx_bufs, tp->tx_bufs_dma);
        dma_free_coherent(&tp->pdev->dev, RX_BUF_TOT_LEN,
                          tp->rx_ring, tp->rx_ring_dma);
        tp->tx_bufs = 0;
        tp->rx_ring = 0;
        tp->dma_ready = false;
        free_irq(dev->irq, dev);
        tp->irq_registered = false;
        return -1;
    }

    netif_start_queue(dev);
    netdev_info(dev,
                "RTL8139 opened: RX ring DMA=0x%llx TX DMA=0x%llx shared IRQ=%u",
                (unsigned long long)tp->rx_ring_dma,
                (unsigned long long)tp->tx_bufs_dma,
                dev->irq);
    return 0;
}

static int rtl8139_close(struct net_device *dev) {
    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    if (tp == 0) return 0;

    netif_stop_queue(dev);
    if (tp->ioaddr != 0) {
        rtl_w16(tp, IntrMask, 0u);
        rtl_w8(tp, ChipCmd, 0u);
    }

    if (tp->irq_registered) {
        free_irq(dev->irq, dev);
        tp->irq_registered = false;
    }

    if (tp->tx_bufs != 0) {
        dma_free_coherent(&tp->pdev->dev, TX_BUF_TOT_LEN,
                          tp->tx_bufs, tp->tx_bufs_dma);
        tp->tx_bufs = 0;
    }
    if (tp->rx_ring != 0) {
        dma_free_coherent(&tp->pdev->dev, RX_BUF_TOT_LEN,
                          tp->rx_ring, tp->rx_ring_dma);
        tp->rx_ring = 0;
    }
    tp->dma_ready = false;
    return 0;
}

static netdev_tx_t rtl8139_start_xmit(struct sk_buff *skb, struct net_device *dev) {
    if (skb == 0 || dev == 0) return NETDEV_TX_BUSY;

    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    if (tp == 0 || !tp->dma_ready) {
        dev_kfree_skb(skb);
        return NETDEV_TX_BUSY;
    }

    unsigned long flags;
    spin_lock_irqsave(&tp->lock, flags);

    if (tp->cur_tx - tp->dirty_tx >= NUM_TX_DESC) {
        netif_stop_queue(dev);
        spin_unlock_irqrestore(&tp->lock, flags);
        return NETDEV_TX_BUSY;
    }

    const unsigned int entry = tp->cur_tx % NUM_TX_DESC;
    const unsigned int length = skb->len;
    if (length == 0u || length > TX_BUF_SIZE) {
        ++dev->stats.tx_dropped;
        spin_unlock_irqrestore(&tp->lock, flags);
        dev_kfree_skb(skb);
        return NETDEV_TX_OK;
    }

    const unsigned int wire_length = length < ETH_ZLEN ? ETH_ZLEN : length;
    if (wire_length > length)
        bytes_zero(tp->tx_buf[entry], wire_length);
    bytes_copy(tp->tx_buf[entry], skb->data, length);
    tp->tx_len[entry] = length;

    wmb();
    rtl_w32_flush(tp,
                  TxStatus0 + entry * 4u,
                  tp->tx_flag | wire_length);

    ++tp->cur_tx;
    dev->trans_start = jiffies;
    if (tp->cur_tx - tp->dirty_tx >= NUM_TX_DESC)
        netif_stop_queue(dev);

    spin_unlock_irqrestore(&tp->lock, flags);
    dev_kfree_skb(skb);
    return NETDEV_TX_OK;
}

static const struct net_device_ops rtl8139_netdev_ops = {
    .ndo_open = rtl8139_open,
    .ndo_stop = rtl8139_close,
    .ndo_start_xmit = rtl8139_start_xmit,
};

static int find_mmio_bar(struct pci_dev *pdev) {
    for (int bar = 0; bar < 6; ++bar) {
        const resource_size_t length = pci_resource_len(pdev, bar);
        const unsigned long flags = pci_resource_flags(pdev, bar);
        if (length >= RTL_MIN_IO_SIZE && (flags & IORESOURCE_MEM) != 0)
            return bar;
    }
    return -1;
}

static int rtl8139_probe(struct pci_dev *pdev, const struct pci_device_id *id) {
    (void)id;
    if (pdev == 0) return -1;

    if (pci_enable_device(pdev) != 0) return -1;
    if (dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32)) != 0) {
        dev_err(&pdev->dev, "RTL8139 requires 32-bit coherent DMA");
        pci_disable_device(pdev);
        return -1;
    }
    if (pci_request_regions(pdev, DRV_NAME) != 0) {
        pci_disable_device(pdev);
        return -1;
    }
    pci_set_master(pdev);

    const int mmio_bar = find_mmio_bar(pdev);
    if (mmio_bar < 0) {
        dev_err(&pdev->dev, "RTL8139 has no usable MMIO BAR");
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    void __iomem *ioaddr = pci_iomap(pdev, mmio_bar, 0);
    if (ioaddr == 0) {
        dev_err(&pdev->dev, "unable to map RTL8139 MMIO BAR%d", mmio_bar);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    struct net_device *dev = alloc_etherdev(sizeof(struct rtl8139_private));
    if (dev == 0) {
        pci_iounmap(pdev, ioaddr);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    tp->pdev = pdev;
    tp->dev = dev;
    tp->ioaddr = ioaddr;
    tp->mmio_bar = mmio_bar;
    spin_lock_init(&tp->lock);

    dev->irq = pdev->irq;
    dev->base_addr = (unsigned long)(uintptr_t)ioaddr;
    dev->netdev_ops = &rtl8139_netdev_ops;

    for (unsigned int i = 0; i < ETH_ALEN; ++i)
        dev->dev_addr[i] = rtl_r8(tp, MAC0 + i);

    if (!is_valid_ether_addr(dev->dev_addr)) {
        dev_err(&pdev->dev,
                "RTL8139 reported invalid MAC %02x:%02x:%02x:%02x:%02x:%02x",
                dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
                dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5]);
        free_netdev(dev);
        pci_iounmap(pdev, ioaddr);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    if (!rtl8139_chip_reset(tp)) {
        dev_err(&pdev->dev, "RTL8139 did not complete reset during probe");
        free_netdev(dev);
        pci_iounmap(pdev, ioaddr);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    if (register_netdev(dev) != 0) {
        free_netdev(dev);
        pci_iounmap(pdev, ioaddr);
        pci_release_regions(pdev);
        pci_disable_device(pdev);
        return -1;
    }

    pci_set_drvdata(pdev, dev);
    netdev_info(dev,
                "Linux 8139too core bound to PCI %04x:%04x BAR%d=%p len=%llu",
                pdev->vendor, pdev->device, mmio_bar, ioaddr,
                (unsigned long long)pci_resource_len(pdev, mmio_bar));
    return 0;
}

static void rtl8139_remove(struct pci_dev *pdev) {
    if (pdev == 0) return;
    struct net_device *dev = (struct net_device *)pci_get_drvdata(pdev);
    if (dev == 0) return;

    struct rtl8139_private *tp = (struct rtl8139_private *)netdev_priv(dev);
    unregister_netdev(dev);
    if (tp != 0 && tp->ioaddr != 0) pci_iounmap(pdev, tp->ioaddr);
    free_netdev(dev);
    pci_set_drvdata(pdev, 0);
    pci_release_regions(pdev);
    pci_disable_device(pdev);
}

static const struct pci_device_id rtl8139_pci_ids[] = {
    { PCI_DEVICE(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID) },
    { 0 }
};
MODULE_DEVICE_TABLE(pci, rtl8139_pci_ids);

static struct pci_driver rtl8139_driver = {
    .name = DRV_NAME,
    .id_table = rtl8139_pci_ids,
    .probe = rtl8139_probe,
    .remove = rtl8139_remove,
};

module_pci_driver(rtl8139_driver);

MODULE_AUTHOR("Linux 8139too authors; Twilight compatibility port");
MODULE_DESCRIPTION("RealTek RTL-8139 Fast Ethernet driver via Twilight Linux compatibility");
MODULE_LICENSE("GPL");
