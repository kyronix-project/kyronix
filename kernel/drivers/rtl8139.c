#include "netdev.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "pci.h"

#define RTL_IDR0 0x00
#define RTL_TSD0 0x10
#define RTL_TSAD0 0x20
#define RTL_RBSTART 0x30
#define RTL_CR 0x37
#define RTL_CAPR 0x38
#define RTL_CBR 0x3A
#define RTL_IMR 0x3C
#define RTL_ISR 0x3E
#define RTL_TCR 0x40
#define RTL_RCR 0x44
#define RTL_CONFIG1 0x52

#define RTL_CR_RST (1u << 4)
#define RTL_CR_RE (1u << 3)
#define RTL_CR_TE (1u << 2)
#define RTL_CR_BUFE (1u << 0)

#define RTL_RCR_AAP (1u << 0)
#define RTL_RCR_APM (1u << 1)
#define RTL_RCR_AM (1u << 2)
#define RTL_RCR_AB (1u << 3)
#define RTL_RCR_WRAP (1u << 7)
#define RTL_RCR_RBLEN_64K (3u << 11)

#define RTL_ISR_ROK (1u << 0)
#define RTL_ISR_TOK (1u << 2)

#define RTL_RX_BUF_SIZE 65536
#define RTL_RX_PAD 2048

typedef struct {
    uint16_t iobase;
    uint8_t *rxbuf;
    uint64_t rxbuf_phys;
    uint8_t *txbuf;
    uint64_t txbuf_phys;
    uint16_t rx_off;
    netdev_t nd;
} rtl8139_t;

static rtl8139_t g_rtl[2];
static int g_nrtl;

static inline void rtl_w8(rtl8139_t *r, uint16_t reg, uint8_t v) { outb(r->iobase + reg, v); }
static inline void rtl_w16(rtl8139_t *r, uint16_t reg, uint16_t v) { outw(r->iobase + reg, v); }
static inline void rtl_w32(rtl8139_t *r, uint16_t reg, uint32_t v) { outl(r->iobase + reg, v); }
static inline uint8_t rtl_r8(rtl8139_t *r, uint16_t reg) { return inb(r->iobase + reg); }
static inline uint16_t rtl_r16(rtl8139_t *r, uint16_t reg) { return inw(r->iobase + reg); }
static inline uint32_t rtl_r32(rtl8139_t *r, uint16_t reg) { return inl(r->iobase + reg); }

static int rtl8139_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    rtl8139_t *r = (rtl8139_t *) nd->priv;
    memcpy(r->txbuf, frame, len);
    rtl_w32(r, RTL_TSAD0, (uint32_t) r->txbuf_phys);
    rtl_w32(r, RTL_TSD0, len);
    uint32_t to = 100000;
    while ((rtl_r32(r, RTL_TSD0) & (1u << 13)) && to--) cpu_relax();
    return to ? 0 : -1;
}

static void rtl8139_poll(netdev_t *nd) {
    rtl8139_t *r = (rtl8139_t *) nd->priv;
    for (;;) {
        uint16_t isr = rtl_r16(r, RTL_ISR);
        rtl_w16(r, RTL_ISR, isr);
        if (!(isr & RTL_ISR_ROK)) break;
        if (rtl_r8(r, RTL_CR) & RTL_CR_BUFE) break;

        uint8_t *pkt = r->rxbuf + r->rx_off;
        uint16_t status = pkt[0] | (pkt[1] << 8);
        uint16_t plen = pkt[2] | (pkt[3] << 8);
        if (!(status & 1) || plen < 14 || plen > 1800) {
            r->rx_off = 0;
            rtl_w16(r, RTL_CAPR, (uint16_t) (r->rx_off - 16));
            break;
        }
        netdev_receive(nd, pkt + 4, (uint16_t) (plen - 4));
        r->rx_off = (uint16_t) ((r->rx_off + plen + 4 + 3) & ~3u);
        rtl_w16(r, RTL_CAPR, (uint16_t) (r->rx_off - 16));
    }
}

static int rtl8139_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nrtl >= 2) return 0;
    uint32_t bar0 = pci_read32(bus, dev, fn, 0x10);
    if (!(bar0 & 1)) return 0;

    rtl8139_t *r = &g_rtl[g_nrtl];
    memset(r, 0, sizeof(*r));
    r->nd.priv = r;
    r->iobase = (uint16_t) (bar0 & 0xFFFCu);

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x05u);

    rtl_w8(r, RTL_CONFIG1, 0);
    rtl_w8(r, RTL_CR, RTL_CR_RST);
    uint32_t to = 100000;
    while ((rtl_r8(r, RTL_CR) & RTL_CR_RST) && to--) cpu_relax();
    if (!to) return 0;

    r->rxbuf_phys = (uint64_t) pmm_alloc_contiguous(
        (RTL_RX_BUF_SIZE + RTL_RX_PAD + PAGE_SIZE - 1) / PAGE_SIZE);
    r->txbuf_phys = (uint64_t) pmm_alloc_contiguous(1);
    if (!r->rxbuf_phys || !r->txbuf_phys) return 0;
    r->rxbuf = (uint8_t *) phys_to_virt(r->rxbuf_phys);
    r->txbuf = (uint8_t *) phys_to_virt(r->txbuf_phys);
    memset(r->rxbuf, 0, RTL_RX_BUF_SIZE + RTL_RX_PAD);

    for (int i = 0; i < 6; i++) r->nd.mac[i] = rtl_r8(r, (uint16_t) (RTL_IDR0 + i));

    rtl_w8(r, RTL_CR, RTL_CR_RE | RTL_CR_TE);
    rtl_w32(r, RTL_RBSTART, (uint32_t) r->rxbuf_phys);
    rtl_w32(r, RTL_RCR, RTL_RCR_AAP | RTL_RCR_APM | RTL_RCR_AM | RTL_RCR_AB | RTL_RCR_WRAP |
                            RTL_RCR_RBLEN_64K);
    rtl_w32(r, RTL_TCR, 0x03000000u);
    rtl_w16(r, RTL_IMR, RTL_ISR_ROK | RTL_ISR_TOK);
    r->rx_off = 0;

    snprintf(r->nd.name, NETDEV_NAME_MAX, "eth%d", g_nrtl + 4);
    r->nd.send = rtl8139_send;
    r->nd.poll = rtl8139_poll;
    netdev_register(&r->nd);
    g_nrtl++;
    return 1;
}

void rtl8139_init(void) {
    g_nrtl = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor == 0x10EC && d->device == 0x8139)
            rtl8139_probe_one(d->bus, d->dev, d->fn);
    }
    if (g_nrtl) log_info("rtl8139: %d NIC(s) initialized", g_nrtl);
}
