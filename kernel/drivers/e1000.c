#include "netdev.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "pci.h"

#define E1000_CTRL 0x0000
#define E1000_STATUS 0x0008
#define E1000_EERD 0x0014
#define E1000_ICR 0x00C0
#define E1000_IMS 0x00D0
#define E1000_RCTL 0x0100
#define E1000_TCTL 0x0400
#define E1000_TIPG 0x0410
#define E1000_RDBAL 0x2800
#define E1000_RDBAH 0x2804
#define E1000_RDLEN 0x2808
#define E1000_RDH 0x2810
#define E1000_RDT 0x2818
#define E1000_TDBAL 0x3800
#define E1000_TDBAH 0x3804
#define E1000_TDLEN 0x3808
#define E1000_TDH 0x3810
#define E1000_TDT 0x3818
#define E1000_MTA 0x5200
#define E1000_RAL 0x5400
#define E1000_RAH 0x5404

#define E1000_CTRL_RST (1u << 26)
#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_ASDE (1u << 5)

#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_SBP (1u << 2)
#define E1000_RCTL_UPE (1u << 3)
#define E1000_RCTL_MPE (1u << 4)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_BSIZE_2048 0
#define E1000_RCTL_SECRC (1u << 26)

#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
#define E1000_TCTL_CT_SHIFT 4
#define E1000_TCTL_COLD_SHIFT 12

#define E1000_RXD_STAT_DD 1
#define E1000_TXD_STAT_DD 1
#define E1000_TXD_CMD_EOP 1
#define E1000_TXD_CMD_IFCS 2
#define E1000_TXD_CMD_RS 8

#define E1000_NUM_RX_DESC 64
#define E1000_NUM_TX_DESC 64
#define E1000_RX_BUF 2048
#define E1000_MMIO_VBASE 0xffff935000000000ULL

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} e1000_rx_desc_t;

typedef struct PACKED {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} e1000_tx_desc_t;

typedef struct {
    volatile uint32_t *regs;
    e1000_rx_desc_t *rx;
    uint64_t rx_phys;
    e1000_tx_desc_t *tx;
    uint64_t tx_phys;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_phys;
    uint8_t *tx_bufs;
    uint64_t tx_bufs_phys;
    uint32_t rx_tail;
    uint32_t tx_tail;
    netdev_t nd;
} e1000_t;

static e1000_t g_e1000[2];
static int g_ne1000;

static inline uint32_t e1000_r(e1000_t *e, uint32_t reg) { return e->regs[reg / 4]; }
static inline void e1000_w(e1000_t *e, uint32_t reg, uint32_t v) { e->regs[reg / 4] = v; }

static void e1000_read_mac(e1000_t *e) {
    uint32_t ral = e1000_r(e, E1000_RAL);
    uint32_t rah = e1000_r(e, E1000_RAH);
    if (ral || (rah & 0xFFFF)) {
        e->nd.mac[0] = ral & 0xFF;
        e->nd.mac[1] = (ral >> 8) & 0xFF;
        e->nd.mac[2] = (ral >> 16) & 0xFF;
        e->nd.mac[3] = (ral >> 24) & 0xFF;
        e->nd.mac[4] = rah & 0xFF;
        e->nd.mac[5] = (rah >> 8) & 0xFF;
        return;
    }
    for (int i = 0; i < 3; i++) {
        e1000_w(e, E1000_EERD, ((uint32_t) i << 8) | 1);
        uint32_t to = 100000;
        while (!(e1000_r(e, E1000_EERD) & (1u << 4)) && to--) cpu_relax();
        uint32_t v = e1000_r(e, E1000_EERD);
        uint16_t w = (uint16_t) (v >> 16);
        e->nd.mac[i * 2] = w & 0xFF;
        e->nd.mac[i * 2 + 1] = (w >> 8) & 0xFF;
    }
}

static int e1000_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    e1000_t *e = (e1000_t *) nd->priv;
    uint32_t t = e->tx_tail;
    e1000_tx_desc_t *d = &e->tx[t];
    memcpy(e->tx_bufs + t * E1000_RX_BUF, frame, len);
    d->addr = e->tx_bufs_phys + t * E1000_RX_BUF;
    d->length = len;
    d->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    d->status = 0;
    __asm__ volatile("" ::: "memory");
    e->tx_tail = (t + 1) % E1000_NUM_TX_DESC;
    e1000_w(e, E1000_TDT, e->tx_tail);
    uint32_t to = 100000;
    while (!(d->status & E1000_TXD_STAT_DD) && to--) cpu_relax();
    return 0;
}

static void e1000_poll(netdev_t *nd) {
    e1000_t *e = (e1000_t *) nd->priv;
    for (;;) {
        uint32_t r = (e->rx_tail + 1) % E1000_NUM_RX_DESC;
        e1000_rx_desc_t *d = &e->rx[r];
        if (!(d->status & E1000_RXD_STAT_DD)) break;
        if (d->length >= 14 && d->length < E1000_RX_BUF)
            netdev_receive(nd, e->rx_bufs + r * E1000_RX_BUF, d->length);
        d->status = 0;
        e1000_w(e, E1000_RDT, r);
        e->rx_tail = r;
    }
}

static int e1000_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_ne1000 >= 2) return 0;
    uint64_t bar0 = 0;
    uint32_t bar_size = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            bar0 = d->bars[0];
            bar_size = d->bar_sizes[0];
            break;
        }
    }
    if (!bar0) return 0;

    e1000_t *e = &g_e1000[g_ne1000];
    memset(e, 0, sizeof(*e));
    e->nd.priv = e;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    uint32_t pages = (bar_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 32) pages = 32;
    if (pages > 256) pages = 256;
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map(&g_kernel_space, E1000_MMIO_VBASE + (uint64_t) g_ne1000 * 0x1000000 +
                                        (uint64_t) i * PAGE_SIZE,
                (bar0 & PAGE_MASK) + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    e->regs = (volatile uint32_t *) (E1000_MMIO_VBASE + (uint64_t) g_ne1000 * 0x1000000 +
                                     (bar0 & (PAGE_SIZE - 1)));

    e1000_w(e, E1000_CTRL, e1000_r(e, E1000_CTRL) | E1000_CTRL_RST);
    usb_msleep(10);
    e1000_w(e, E1000_IMS, 0);
    e1000_r(e, E1000_ICR);
    e1000_w(e, E1000_CTRL, e1000_r(e, E1000_CTRL) | E1000_CTRL_SLU);

    e->rx_phys = (uint64_t) pmm_alloc_zeroed();
    e->tx_phys = (uint64_t) pmm_alloc_zeroed();
    if (!e->rx_phys || !e->tx_phys) return 0;
    e->rx = (e1000_rx_desc_t *) phys_to_virt(e->rx_phys);
    e->tx = (e1000_tx_desc_t *) phys_to_virt(e->tx_phys);

    uint32_t rx_pages = (E1000_NUM_RX_DESC * E1000_RX_BUF + PAGE_SIZE - 1) / PAGE_SIZE;
    e->rx_bufs_phys = (uint64_t) pmm_alloc_contiguous(rx_pages);
    e->tx_bufs_phys = (uint64_t) pmm_alloc_contiguous(rx_pages);
    if (!e->rx_bufs_phys || !e->tx_bufs_phys) return 0;
    e->rx_bufs = (uint8_t *) phys_to_virt(e->rx_bufs_phys);
    e->tx_bufs = (uint8_t *) phys_to_virt(e->tx_bufs_phys);

    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e->rx[i].addr = e->rx_bufs_phys + (uint64_t) i * E1000_RX_BUF;
        e->rx[i].status = 0;
    }

    e1000_w(e, E1000_RDBAL, (uint32_t) e->rx_phys);
    e1000_w(e, E1000_RDBAH, (uint32_t) (e->rx_phys >> 32));
    e1000_w(e, E1000_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_w(e, E1000_RDH, 0);
    e1000_w(e, E1000_RDT, E1000_NUM_RX_DESC - 1);
    e->rx_tail = E1000_NUM_RX_DESC - 1;

    e1000_w(e, E1000_TDBAL, (uint32_t) e->tx_phys);
    e1000_w(e, E1000_TDBAH, (uint32_t) (e->tx_phys >> 32));
    e1000_w(e, E1000_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_w(e, E1000_TDH, 0);
    e1000_w(e, E1000_TDT, 0);
    e->tx_tail = 0;

    for (int i = 0; i < 32; i++) e1000_w(e, E1000_MTA + i * 4, 0);

    e1000_read_mac(e);
    e1000_w(e, E1000_RAH, e1000_r(e, E1000_RAH) | (1u << 31));

    e1000_w(e, E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE | E1000_RCTL_BAM |
                               E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);
    e1000_w(e, E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | (0x10u << E1000_TCTL_CT_SHIFT) |
                               (0x200u << E1000_TCTL_COLD_SHIFT));
    e1000_w(e, E1000_TIPG, (10u << 0) | (8u << 10) | (10u << 20));

    snprintf(e->nd.name, NETDEV_NAME_MAX, "eth%d", g_ne1000);
    e->nd.send = e1000_send;
    e->nd.poll = e1000_poll;
    netdev_register(&e->nd);
    g_ne1000++;
    return 1;
}

static const struct {
    uint16_t dev;
    const char *name;
} g_e1000_ids[] = {
    {0x100E, "82540EM"}, {0x100F, "82545EM"}, {0x1010, "82546EB"}, {0x1011, "82545EM"},
    {0x1012, "82546EB"}, {0x1013, "82541EI"}, {0x1014, "82541ER"}, {0x1015, "82540EM"},
    {0x1016, "82540EP"}, {0x1017, "82540EM"}, {0x1018, "82541EI"}, {0x1019, "82547EI"},
    {0x101A, "82547EI"}, {0x101D, "82546EB"}, {0x101E, "82540EP"}, {0x1026, "82545GM"},
    {0x1027, "82545GM"}, {0x1028, "82545GM"}, {0x1049, "82566MM"}, {0x104A, "82566DM"},
    {0x104B, "82566DC"}, {0x104C, "82562V"}, {0x104D, "82566MC"}, {0x10A4, "82571EB"},
    {0x10A5, "82571EB"}, {0x10B9, "82572EI"}, {0x10BC, "82571EB"}, {0x10BD, "82566DM-2"},
    {0x10C0, "82562V-2"}, {0x10C2, "82562G-2"}, {0x10C3, "82562GT-2"}, {0x10C4, "82562GT"},
    {0x10C5, "82562G"}, {0x10CB, "82567V"}, {0x10CC, "82567LM-2"}, {0x10CD, "82567LF-2"},
    {0x10CE, "82567V-2"}, {0x10D3, "82574L"}, {0x10D9, "82571EB"}, {0x10DA, "82571EB"},
    {0x10DE, "82567LM-3"}, {0x10DF, "82567LF-3"}, {0x10E5, "82567LM-4"}, {0x10EA, "82577LM"},
    {0x10EB, "82577LC"}, {0x10EF, "82578DM"}, {0x10F0, "82578DC"}, {0x10F5, "82567LM"},
    {0x10F6, "82574LA"}, {0x1501, "82567V-3"}, {0x1502, "82579LM"}, {0x1503, "82579V"},
    {0x150A, "82576NS"}, {0x150C, "82583V"}, {0x150E, "82580"}, {0x150F, "82576"},
    {0x1516, "82580"}, {0x1518, "82576"}, {0x1521, "i350"}, {0x1522, "i350"}, {0x1523, "i350"},
    {0x1524, "i350"}, {0x1533, "i210"}, {0x1539, "i211"}, {0x157B, "i210"}, {0x153A, "i217-LM"},
    {0x153B, "i217-V"}, {0x155A, "i218-LM"}, {0x1559, "i218-V"}, {0x15A0, "i218-LM"},
    {0x15A1, "i218-V"}, {0x15A2, "i218-LM"}, {0x15A3, "i218-V"}, {0x156F, "i219-LM"},
    {0x1570, "i219-V"}, {0x15B7, "i219-LM"}, {0x15B8, "i219-V"}, {0x15B9, "i219-LM"},
    {0x15D7, "i219-LM"}, {0x15D8, "i219-V"}, {0x15E3, "i219-LM"}, {0x0D4F, "i219-LM"},
    {0, NULL},
};

void e1000_init(void) {
    g_ne1000 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor != 0x8086 || d->class != 0x02 || d->subclass != 0x00) continue;
        for (int j = 0; g_e1000_ids[j].dev; j++) {
            if (g_e1000_ids[j].dev == d->device) {
                log_info("e1000: found Intel %s (%04x:%04x)", g_e1000_ids[j].name, d->vendor,
                         d->device);
                e1000_probe_one(d->bus, d->dev, d->fn);
                break;
            }
        }
    }
    if (g_ne1000) log_info("e1000: %d NIC(s) initialized", g_ne1000);
}
