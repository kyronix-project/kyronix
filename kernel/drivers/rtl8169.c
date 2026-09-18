#include "netdev.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "pci.h"

#define R9_IDR0 0x00
#define R9_MAR0 0x08
#define R9_TNPDS 0x20
#define R9_THPDS 0x28
#define R9_RDSAR 0x30
#define R9_CR 0x37
#define R9_TPPOLL 0x38
#define R9_IMR 0x3C
#define R9_ISR 0x3E
#define R9_TCR 0x40
#define R9_RCR 0x44
#define R9_RMS 0x46
#define R9_9356CR 0x50
#define R9_PHYAR 0x60
#define R9_TBICSR 0x64
#define R9_CPCR 0xE0
#define R9_RDSAR_LO 0xE4
#define R9_RDSAR_HI 0xE8

#define R9_CR_RST (1u << 4)
#define R9_CR_RE (1u << 3)
#define R9_CR_TE (1u << 2)

#define R9_TPPOLL_NPQ (1u << 6)

#define R9_ISR_ROK (1u << 0)
#define R9_ISR_TOK (1u << 2)
#define R9_ISR_RDU (1u << 5)
#define R9_ISR_TDU (1u << 7)

#define R9_RCR_AAP (1u << 0)
#define R9_RCR_APM (1u << 1)
#define R9_RCR_AM (1u << 2)
#define R9_RCR_AB (1u << 3)
#define R9_RCR_MXDMA_UNLIMITED (7u << 8)
#define R9_RCR_RXFTH_UNLIMITED (7u << 13)

#define R9_TCR_MXDMA_UNLIMITED (7u << 8)
#define R9_TCR_IFG_NORMAL (3u << 24)

#define R9_CPCR_RXVLAN (1u << 6)
#define R9_CPCR_RXCKSUM (1u << 5)

#define R9_9356CR_EEM (3u << 6)

#define R9_DESC_OWN (1u << 31)
#define R9_DESC_EOR (1u << 30)
#define R9_DESC_FS (1u << 29)
#define R9_DESC_LS (1u << 28)

#define R9_NUM_RX_DESC 64
#define R9_NUM_TX_DESC 64
#define R9_RX_BUF 2048
#define R9_MMIO_VBASE 0xffff936000000000ULL

typedef struct PACKED {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} r9_desc_t;

typedef struct {
    volatile uint32_t *regs;
    uint64_t regs_phys;
    r9_desc_t *rx;
    uint64_t rx_phys;
    r9_desc_t *tx;
    uint64_t tx_phys;
    uint8_t *rx_bufs;
    uint64_t rx_bufs_phys;
    uint8_t *tx_bufs;
    uint64_t tx_bufs_phys;
    uint32_t rx_tail;
    uint32_t tx_tail;
    netdev_t nd;
} rtl8169_t;

static rtl8169_t g_r9[2];
static int g_nr9;

static inline uint32_t r9_r32(rtl8169_t *r, uint32_t reg) { return r->regs[reg / 4]; }
static inline void r9_w32(rtl8169_t *r, uint32_t reg, uint32_t v) { r->regs[reg / 4] = v; }
static inline uint8_t r9_r8(rtl8169_t *r, uint32_t reg) {
    return *(volatile uint8_t *) ((volatile uint8_t *) r->regs + reg);
}
static inline void r9_w8(rtl8169_t *r, uint32_t reg, uint8_t v) {
    *(volatile uint8_t *) ((volatile uint8_t *) r->regs + reg) = v;
}
static inline uint16_t r9_r16(rtl8169_t *r, uint32_t reg) {
    return *(volatile uint16_t *) ((volatile uint8_t *) r->regs + reg);
}
static inline void r9_w16(rtl8169_t *r, uint32_t reg, uint16_t v) {
    *(volatile uint16_t *) ((volatile uint8_t *) r->regs + reg) = v;
}

static int rtl8169_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    rtl8169_t *r = (rtl8169_t *) nd->priv;
    uint32_t t = r->tx_tail;
    memcpy(r->tx_bufs + t * R9_RX_BUF, frame, len);
    r->tx[t].addr = r->tx_bufs_phys + t * R9_RX_BUF;
    r->tx[t].opts1 = R9_DESC_OWN | R9_DESC_FS | R9_DESC_LS | len;
    if (t == R9_NUM_TX_DESC - 1) r->tx[t].opts1 |= R9_DESC_EOR;
    __asm__ volatile("" ::: "memory");
    r->tx_tail = (t + 1) % R9_NUM_TX_DESC;
    r9_w8(r, R9_TPPOLL, R9_TPPOLL_NPQ);
    uint32_t to = 100000;
    while ((r->tx[t].opts1 & R9_DESC_OWN) && to--) cpu_relax();
    return to ? 0 : -1;
}

static void rtl8169_poll(netdev_t *nd) {
    rtl8169_t *r = (rtl8169_t *) nd->priv;
    uint16_t isr = r9_r16(r, R9_ISR);
    r9_w16(r, R9_ISR, isr);
    for (;;) {
        r9_desc_t *d = &r->rx[r->rx_tail];
        if (d->opts1 & R9_DESC_OWN) break;
        uint32_t len = d->opts1 & 0x3FFFu;
        if (len >= 14 && len < R9_RX_BUF)
            netdev_receive(nd, r->rx_bufs + r->rx_tail * R9_RX_BUF, (uint16_t) (len - 4));
        d->opts1 = R9_DESC_OWN | R9_RX_BUF;
        if (r->rx_tail == R9_NUM_RX_DESC - 1) d->opts1 |= R9_DESC_EOR;
        __asm__ volatile("" ::: "memory");
        r->rx_tail = (r->rx_tail + 1) % R9_NUM_RX_DESC;
    }
}

static int rtl8169_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nr9 >= 2) return 0;
    uint64_t bar2 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            bar2 = d->bars[2];
            break;
        }
    }
    if (!bar2) return 0;

    rtl8169_t *r = &g_r9[g_nr9];
    memset(r, 0, sizeof(*r));
    r->nd.priv = r;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    for (int i = 0; i < 8; i++) {
        vmm_map(&g_kernel_space, R9_MMIO_VBASE + (uint64_t) g_nr9 * 0x1000000 +
                                        (uint64_t) i * PAGE_SIZE,
                (bar2 & PAGE_MASK) + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    r->regs = (volatile uint32_t *) (R9_MMIO_VBASE + (uint64_t) g_nr9 * 0x1000000 +
                                     (bar2 & (PAGE_SIZE - 1)));

    r9_w8(r, R9_9356CR, (uint8_t) (r9_r8(r, R9_9356CR) | R9_9356CR_EEM));
    r9_w8(r, R9_CR, R9_CR_RST);
    uint32_t to = 100000;
    while ((r9_r8(r, R9_CR) & R9_CR_RST) && to--) cpu_relax();
    if (!to) return 0;

    r->rx_phys = (uint64_t) pmm_alloc_zeroed();
    r->tx_phys = (uint64_t) pmm_alloc_zeroed();
    if (!r->rx_phys || !r->tx_phys) return 0;
    r->rx = (r9_desc_t *) phys_to_virt(r->rx_phys);
    r->tx = (r9_desc_t *) phys_to_virt(r->tx_phys);

    uint32_t rx_pages = (R9_NUM_RX_DESC * R9_RX_BUF + PAGE_SIZE - 1) / PAGE_SIZE;
    r->rx_bufs_phys = (uint64_t) pmm_alloc_contiguous(rx_pages);
    r->tx_bufs_phys = (uint64_t) pmm_alloc_contiguous(rx_pages);
    if (!r->rx_bufs_phys || !r->tx_bufs_phys) return 0;
    r->rx_bufs = (uint8_t *) phys_to_virt(r->rx_bufs_phys);
    r->tx_bufs = (uint8_t *) phys_to_virt(r->tx_bufs_phys);

    for (int i = 0; i < R9_NUM_RX_DESC; i++) {
        r->rx[i].addr = r->rx_bufs_phys + (uint64_t) i * R9_RX_BUF;
        r->rx[i].opts1 = R9_DESC_OWN | R9_RX_BUF;
        if (i == R9_NUM_RX_DESC - 1) r->rx[i].opts1 |= R9_DESC_EOR;
    }
    for (int i = 0; i < R9_NUM_TX_DESC; i++) {
        r->tx[i].addr = r->tx_bufs_phys + (uint64_t) i * R9_RX_BUF;
        r->tx[i].opts1 = 0;
        if (i == R9_NUM_TX_DESC - 1) r->tx[i].opts1 |= R9_DESC_EOR;
    }

    for (int i = 0; i < 6; i++) r->nd.mac[i] = r9_r8(r, (uint32_t) (R9_IDR0 + i));

    r9_w32(r, R9_RDSAR, (uint32_t) r->rx_phys);
    r9_w32(r, R9_RDSAR + 4, (uint32_t) (r->rx_phys >> 32));
    r9_w32(r, R9_TNPDS, (uint32_t) r->tx_phys);
    r9_w32(r, R9_TNPDS + 4, (uint32_t) (r->tx_phys >> 32));

    r9_w32(r, R9_RCR, R9_RCR_AAP | R9_RCR_APM | R9_RCR_AM | R9_RCR_AB |
                          R9_RCR_MXDMA_UNLIMITED | R9_RCR_RXFTH_UNLIMITED);
    r9_w32(r, R9_TCR, R9_TCR_MXDMA_UNLIMITED | R9_TCR_IFG_NORMAL);
    r9_w16(r, R9_RMS, 0x1FFF);
    r9_w16(r, R9_CPCR, R9_CPCR_RXCKSUM);

    r9_w8(r, R9_CR, R9_CR_RE | R9_CR_TE);
    r9_w16(r, R9_IMR, R9_ISR_ROK | R9_ISR_TOK | R9_ISR_RDU | R9_ISR_TDU);
    r9_w16(r, R9_ISR, 0xFFFF);

    r->rx_tail = 0;
    r->tx_tail = 0;

    snprintf(r->nd.name, NETDEV_NAME_MAX, "eth%d", g_nr9 + 6);
    r->nd.send = rtl8169_send;
    r->nd.poll = rtl8169_poll;
    netdev_register(&r->nd);
    g_nr9++;
    return 1;
}

void rtl8169_init(void) {
    g_nr9 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor == 0x10EC &&
            (d->device == 0x8167 || d->device == 0x8168 || d->device == 0x8169 ||
             d->device == 0x8136))
            rtl8169_probe_one(d->bus, d->dev, d->fn);
    }
    if (g_nr9) log_info("rtl8169: %d NIC(s) initialized", g_nr9);
}
