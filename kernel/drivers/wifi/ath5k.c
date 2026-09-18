#include "ieee80211.h"
#include "../../arch/x86_64/cpu.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci.h"
#include "../usb/usb.h"

#define AR5K_CR 0x0000
#define AR5K_RXDP 0x000C
#define AR5K_CFG 0x0014
#define AR5K_IER 0x0024
#define AR5K_TXDP0 0x0800
#define AR5K_TXDP1 0x0804
#define AR5K_IMR 0x0080
#define AR5K_ISR 0x001C
#define AR5K_PISR 0x0080
#define AR5K_RX_STATUS 0x0C00
#define AR5K_TX_STATUS0 0x0C80
#define AR5K_RX_FILTER 0x0C04
#define AR5K_BSS_ID0 0x8020
#define AR5K_BSS_ID1 0x8024
#define AR5K_STA_ID0 0x8000
#define AR5K_STA_ID1 0x8004
#define AR5K_USEC 0x801C
#define AR5K_BEACON 0x8028
#define AR5K_CFP_PERIOD 0x8030
#define AR5K_CFP_DUR 0x8034
#define AR5K_RX_FILTER_5212 0x81F4
#define AR5K_PHY 0x9800
#define AR5K_PHY_TURBO 0x9804
#define AR5K_PHY_AGC 0x9860
#define AR5K_PHY_CHIP_ID 0x9818
#define AR5K_PHY_ACTIVE 0x981C
#define AR5K_PHY_PLL 0x987C
#define AR5K_RF_BUFFER 0x989C
#define AR5K_RF_BUFFER_CONTROL_4 0x98A4
#define AR5K_RF_BUFFER_CONTROL_5 0x98A8
#define AR5K_EEPROM_BASE 0x6000
#define AR5K_EEPROM_CMD 0x6004
#define AR5K_EEPROM_DATA 0x6008
#define AR5K_EEPROM_STATUS 0x600C
#define AR5K_EEPROM_CFG 0x6010

#define AR5K_CR_RXE 0x00000004
#define AR5K_CR_RXD 0x00000008
#define AR5K_CR_SWI 0x00000040

#define AR5K_IER_ENABLE 0x00000001
#define AR5K_IER_DISABLE 0x00000000

#define AR5K_ISR_RXOK 0x00000001
#define AR5K_ISR_RXDESC 0x00000002
#define AR5K_ISR_RXERR 0x00000004
#define AR5K_ISR_RXNOPKT 0x00000008
#define AR5K_ISR_RXEOL 0x00000010
#define AR5K_ISR_RXORN 0x00000020
#define AR5K_ISR_TXOK 0x00000040
#define AR5K_ISR_TXDESC 0x00000080
#define AR5K_ISR_TXERR 0x00000100
#define AR5K_ISR_TXNOPKT 0x00000200
#define AR5K_ISR_TXEOL 0x00000400
#define AR5K_ISR_TXURN 0x00000800
#define AR5K_ISR_MIB 0x00001000
#define AR5K_ISR_SWI 0x00002000
#define AR5K_ISR_RXPHY 0x00004000
#define AR5K_ISR_RXKCM 0x00008000
#define AR5K_ISR_SWBA 0x00010000
#define AR5K_ISR_BRSSI 0x00020000
#define AR5K_ISR_BMISS 0x00040000
#define AR5K_ISR_FATAL 0x00080000
#define AR5K_ISR_BNR 0x00100000
#define AR5K_ISR_RXCHIRP 0x00200000
#define AR5K_ISR_RXDOPPLER 0x00400000
#define AR5K_ISR_QCBORN 0x00800000
#define AR5K_ISR_QCBURN 0x01000000
#define AR5K_ISR_QTRIG 0x02000000

#define AR5K_RX_FILTER_UCAST 0x00000001
#define AR5K_RX_FILTER_MCAST 0x00000002
#define AR5K_RX_FILTER_BCAST 0x00000004
#define AR5K_RX_FILTER_CONTROL 0x00000008
#define AR5K_RX_FILTER_BEACON 0x00000010
#define AR5K_RX_FILTER_PROM 0x00000020
#define AR5K_RX_FILTER_XRPOLL 0x00000040
#define AR5K_RX_FILTER_PROBEREQ 0x00000080
#define AR5K_RX_FILTER_PHYERR_5212 0x00000100
#define AR5K_RX_FILTER_RADARERR_5212 0x00000200

#define AR5K_STA_ID1_AP 0x00010000
#define AR5K_STA_ID1_ADHOC 0x00020000
#define AR5K_STA_ID1_PWR_SV 0x00040000
#define AR5K_STA_ID1_NO_KEYSRCH 0x00080000
#define AR5K_STA_ID1_NO_PSPOLL 0x00100000
#define AR5K_STA_ID1_RTS_DEF_11B 0x00200000
#define AR5K_STA_ID1_DESC_ANTENNA 0x00400000
#define AR5K_STA_ID1_RTS_ANTENNA 0x00800000
#define AR5K_STA_ID1_ACKCTS_6MB 0x01000000
#define AR5K_STA_ID1_BASE_RATE_11B 0x02000000

#define AR5K_EEPROM_READ 0x00000001
#define AR5K_EEPROM_WRITE 0x00000002
#define AR5K_EEPROM_RESET 0x00000004

#define AR5K_EEPROM_STAT_RDERR 0x00000001
#define AR5K_EEPROM_STAT_RDDONE 0x00000002
#define AR5K_EEPROM_STAT_WRERR 0x00000004
#define AR5K_EEPROM_STAT_WRDONE 0x00000008

#define AR5K_NUM_RX_DESC 32
#define AR5K_NUM_TX_DESC 16
#define AR5K_RX_BUF_SIZE 2400
#define AR5K_TX_BUF_SIZE 2400
#define AR5K_MMIO_VBASE 0xffff937000000000ULL

typedef struct PACKED {
    uint32_t link;
    uint32_t data;
    uint32_t ctrl0;
    uint32_t ctrl1;
    uint32_t ctrl2;
    uint32_t ctrl3;
    uint32_t status0;
    uint32_t status1;
} ath5k_desc_t;

typedef struct {
    volatile uint32_t *regs;
    uint16_t devid;
    ath5k_desc_t *rx;
    uint64_t rx_phys;
    ath5k_desc_t *tx;
    uint64_t tx_phys;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_phys;
    uint8_t *tx_bufs;
    uint64_t tx_bufs_phys;
    uint32_t rx_tail;
    uint32_t tx_head;
    uint32_t tx_tail;
    wifi_t wifi;
    int cur_channel;
} ath5k_t;

static ath5k_t g_ath5k[2];
static int g_nath5k;

static inline uint32_t ath5k_r(ath5k_t *a, uint32_t reg) { return a->regs[reg / 4]; }
static inline void ath5k_w(ath5k_t *a, uint32_t reg, uint32_t v) { a->regs[reg / 4] = v; }

static uint16_t ath5k_eeprom_read(ath5k_t *a, uint16_t offset) {
    ath5k_w(a, AR5K_EEPROM_BASE, offset);
    ath5k_w(a, AR5K_EEPROM_CMD, AR5K_EEPROM_READ);
    uint32_t to = 10000;
    while (!(ath5k_r(a, AR5K_EEPROM_STATUS) & AR5K_EEPROM_STAT_RDDONE) && to--) cpu_relax();
    if (!to) return 0xFFFF;
    return (uint16_t) ath5k_r(a, AR5K_EEPROM_DATA);
}

static void ath5k_read_mac(ath5k_t *a) {
    for (int i = 0; i < 3; i++) {
        uint16_t w = ath5k_eeprom_read(a, (uint16_t) (0x14 + i));
        a->wifi.nd.mac[i * 2] = (uint8_t) (w >> 8);
        a->wifi.nd.mac[i * 2 + 1] = (uint8_t) w;
    }
    ath5k_w(a, AR5K_STA_ID0,
            (uint32_t) a->wifi.nd.mac[0] | ((uint32_t) a->wifi.nd.mac[1] << 8) |
                ((uint32_t) a->wifi.nd.mac[2] << 16) | ((uint32_t) a->wifi.nd.mac[3] << 24));
    ath5k_w(a, AR5K_STA_ID1,
            (uint32_t) a->wifi.nd.mac[4] | ((uint32_t) a->wifi.nd.mac[5] << 8) |
                AR5K_STA_ID1_AP | AR5K_STA_ID1_NO_PSPOLL);
}

static int ath5k_set_channel(void *priv, int channel) {
    ath5k_t *a = (ath5k_t *) priv;
    a->cur_channel = channel;
    uint32_t freq = 2407 + (uint32_t) channel * 5;
    ath5k_w(a, AR5K_PHY_ACTIVE, 1);
    usb_msleep(1);
    ath5k_w(a, AR5K_PHY_ACTIVE, 0);
    ath5k_w(a, AR5K_PHY, (freq / 10) | (1u << 10));
    usb_msleep(5);
    return 0;
}

static int ath5k_tx_frame(void *priv, const uint8_t *frame, uint16_t len) {
    ath5k_t *a = (ath5k_t *) priv;
    uint32_t t = a->tx_head;
    ath5k_desc_t *d = &a->tx[t];
    if (d->ctrl0 & 0x80000000u) return -1;
    memcpy(a->tx_bufs + t * AR5K_TX_BUF_SIZE, frame, len);
    d->data = a->tx_bufs_phys + t * AR5K_TX_BUF_SIZE;
    d->ctrl0 = (1u << 0) | ((uint32_t) len << 16) | 0x80000000u;
    d->ctrl1 = (1u << 0) | ((uint32_t) (a->cur_channel * 5 + 2407) << 10);
    d->status0 = 0;
    d->status1 = 0;
    __asm__ volatile("" ::: "memory");
    a->tx_head = (t + 1) % AR5K_NUM_TX_DESC;
    ath5k_w(a, AR5K_TXDP0, a->tx_phys + t * sizeof(ath5k_desc_t));
    ath5k_w(a, AR5K_CR, AR5K_CR_RXE);
    uint32_t to = 100000;
    while ((d->ctrl0 & 0x80000000u) && to--) cpu_relax();
    return 0;
}

static void ath5k_poll_rx(ath5k_t *a) {
    for (;;) {
        ath5k_desc_t *d = &a->rx[a->rx_tail];
        if (d->ctrl0 & 0x80000000u) break;
        uint32_t status = d->status0;
        uint16_t len = (uint16_t) (status & 0xFFF);
        if ((status & (1u << 15)) && len >= 24 && len < AR5K_RX_BUF_SIZE) {
            wifi_rx_frame(&a->wifi, a->rx_bufs + a->rx_tail * AR5K_RX_BUF_SIZE, len);
        }
        d->ctrl0 = 0x80000000u;
        d->status0 = 0;
        __asm__ volatile("" ::: "memory");
        a->rx_tail = (a->rx_tail + 1) % AR5K_NUM_RX_DESC;
    }
}

static int ath5k_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nath5k >= 2) return 0;
    uint64_t bar0 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            bar0 = d->bars[0];
            break;
        }
    }
    if (!bar0) return 0;

    ath5k_t *a = &g_ath5k[g_nath5k];
    memset(a, 0, sizeof(*a));

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    for (int i = 0; i < 32; i++) {
        vmm_map(&g_kernel_space, AR5K_MMIO_VBASE + (uint64_t) g_nath5k * 0x1000000 +
                                        (uint64_t) i * PAGE_SIZE,
                (bar0 & PAGE_MASK) + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    a->regs = (volatile uint32_t *) (AR5K_MMIO_VBASE + (uint64_t) g_nath5k * 0x1000000 +
                                     (bar0 & (PAGE_SIZE - 1)));

    ath5k_w(a, AR5K_IER, AR5K_IER_DISABLE);
    ath5k_w(a, AR5K_ISR, 0xFFFFFFFFu);

    a->rx_phys = (uint64_t) pmm_alloc_zeroed();
    a->tx_phys = (uint64_t) pmm_alloc_zeroed();
    if (!a->rx_phys || !a->tx_phys) return 0;
    a->rx = (ath5k_desc_t *) phys_to_virt(a->rx_phys);
    a->tx = (ath5k_desc_t *) phys_to_virt(a->tx_phys);

    uint32_t rx_pages = (AR5K_NUM_RX_DESC * AR5K_RX_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t tx_pages = (AR5K_NUM_TX_DESC * AR5K_TX_BUF_SIZE + PAGE_SIZE - 1) / PAGE_SIZE;
    a->rx_bufs_phys = (uint64_t) pmm_alloc_contiguous(rx_pages);
    a->tx_bufs_phys = (uint64_t) pmm_alloc_contiguous(tx_pages);
    if (!a->rx_bufs_phys || !a->tx_bufs_phys) return 0;
    a->rx_bufs = (uint8_t *) phys_to_virt(a->rx_bufs_phys);
    a->tx_bufs = (uint8_t *) phys_to_virt(a->tx_bufs_phys);

    for (int i = 0; i < AR5K_NUM_RX_DESC; i++) {
        a->rx[i].link = a->rx_phys + ((i + 1) % AR5K_NUM_RX_DESC) * sizeof(ath5k_desc_t);
        a->rx[i].data = a->rx_bufs_phys + (uint64_t) i * AR5K_RX_BUF_SIZE;
        a->rx[i].ctrl0 = 0x80000000u;
        a->rx[i].status0 = 0;
    }
    for (int i = 0; i < AR5K_NUM_TX_DESC; i++) {
        a->tx[i].link = a->tx_phys + ((i + 1) % AR5K_NUM_TX_DESC) * sizeof(ath5k_desc_t);
        a->tx[i].ctrl0 = 0;
    }

    ath5k_w(a, AR5K_RXDP, (uint32_t) a->rx_phys);
    ath5k_w(a, AR5K_RX_FILTER, AR5K_RX_FILTER_UCAST | AR5K_RX_FILTER_MCAST |
                                   AR5K_RX_FILTER_BCAST | AR5K_RX_FILTER_BEACON |
                                   AR5K_RX_FILTER_PROBEREQ | AR5K_RX_FILTER_PROM);

    ath5k_read_mac(a);

    static wifi_hw_ops_t hw_ops[2];
    hw_ops[g_nath5k].tx = ath5k_tx_frame;
    hw_ops[g_nath5k].set_channel = ath5k_set_channel;
    hw_ops[g_nath5k].hw_priv = a;

    char name[NETDEV_NAME_MAX];
    snprintf(name, NETDEV_NAME_MAX, "wlan%d", g_nath5k);
    wifi_init_dev(&a->wifi, name, a->wifi.nd.mac, &hw_ops[g_nath5k]);
    a->wifi.nd.priv = a;
    netdev_register(&a->wifi.nd);

    ath5k_set_channel(a, 6);
    ath5k_w(a, AR5K_IMR, AR5K_ISR_RXOK | AR5K_ISR_RXERR | AR5K_ISR_TXOK | AR5K_ISR_TXERR);
    ath5k_w(a, AR5K_IER, AR5K_IER_ENABLE);

    log_info("ath5k: %04x:%04x MAC %02x:%02x:%02x:%02x:%02x:%02x", a->devid, 0, a->wifi.nd.mac[0],
             a->wifi.nd.mac[1], a->wifi.nd.mac[2], a->wifi.nd.mac[3], a->wifi.nd.mac[4],
             a->wifi.nd.mac[5]);
    g_nath5k++;
    return 1;
}

static const struct {
    uint16_t dev;
    const char *name;
} g_ath5k_ids[] = {
    {0x0207, "AR5210"}, {0x0007, "AR5210"}, {0x0013, "AR5211"}, {0x0012, "AR5211"},
    {0x001A, "AR5212"}, {0x001B, "AR5213"}, {0x001C, "AR5213A"}, {0x001D, "AR5212"},
    {0x1014, "AR5212"}, {0x111A, "AR5212"}, {0xFF16, "AR5212"}, {0xFF1A, "AR5212"},
    {0xFF1B, "AR5213"}, {0xFF96, "AR5212"}, {0x0013, "AR5212"}, {0x0023, "AR5413"},
    {0x0024, "AR5414"}, {0x0025, "AR5416"}, {0x0027, "AR5416"}, {0x002A, "AR5418"},
    {0x002B, "AR5418"}, {0x002C, "AR2425"}, {0x002D, "AR5416"}, {0x002E, "AR9160"},
    {0, NULL},
};

void ath5k_init(void) {
    g_nath5k = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor != 0x168C) continue;
        for (int j = 0; g_ath5k_ids[j].dev; j++) {
            if (g_ath5k_ids[j].dev == d->device) {
                log_info("ath5k: found Atheros %s (%04x:%04x)", g_ath5k_ids[j].name, d->vendor,
                         d->device);
                ath5k_probe_one(d->bus, d->dev, d->fn);
                break;
            }
        }
    }
    if (g_nath5k) log_info("ath5k: %d WiFi card(s) initialized", g_nath5k);
}
