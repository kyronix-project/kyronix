#include "usb.h"
#include "../../arch/x86_64/cpu.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../pci.h"

#define UHCI_USBCMD 0x00
#define UHCI_USBSTS 0x02
#define UHCI_USBINTR 0x04
#define UHCI_FRNUM 0x06
#define UHCI_FLBASEADD 0x08
#define UHCI_SOFMOD 0x0C
#define UHCI_PORTSC1 0x10
#define UHCI_PORTSC2 0x12

#define UHCI_CMD_RUN 0x0001
#define UHCI_CMD_HCRESET 0x0002
#define UHCI_CMD_GRESET 0x0004
#define UHCI_CMD_MAXP 0x0080

#define UHCI_STS_HCHALTED 0x0020

#define UHCI_PORT_CCS 0x0001
#define UHCI_PORT_CSC 0x0002
#define UHCI_PORT_PED 0x0004
#define UHCI_PORT_PEDC 0x0008
#define UHCI_PORT_LSDA 0x0100
#define UHCI_PORT_RD 0x0200
#define UHCI_PORT_PR 0x0400

#define TD_LINK_TERMINATE 0x00000001u
#define TD_LINK_QH (1u << 1)

#define TD_CTRL_SPD (1u << 29)
#define TD_CTRL_ERR_SHIFT 27
#define TD_CTRL_LS (1u << 26)
#define TD_CTRL_IOS (1u << 25)
#define TD_CTRL_IOC (1u << 24)
#define TD_CTRL_ACTIVE (1u << 23)
#define TD_CTRL_STALLED (1u << 22)
#define TD_CTRL_DATABUFFER (1u << 21)
#define TD_CTRL_BABBLE (1u << 20)
#define TD_CTRL_NAK (1u << 19)
#define TD_CTRL_CRCTO (1u << 18)
#define TD_CTRL_BITSTUFF (1u << 17)
#define TD_CTRL_ACTLEN_MASK 0x7FFu

#define TD_TOKEN_PID_SHIFT 0
#define TD_TOKEN_DEVADDR_SHIFT 8
#define TD_TOKEN_ENDPT_SHIFT 15
#define TD_TOKEN_DT_SHIFT 19
#define TD_TOKEN_MAXLEN_SHIFT 21

#define USB_PID_SETUP 0x2D
#define USB_PID_IN 0x69
#define USB_PID_OUT 0xE1

#define UHCI_NUM_TDS 256
#define UHCI_FRAME_COUNT 1024

typedef struct PACKED {
    uint32_t link;
    uint32_t ctrl;
    uint32_t token;
    uint32_t buffer;
    uint32_t pad[4];
} uhci_td_t;

typedef struct PACKED {
    uint32_t head;
    uint32_t element;
    uint32_t pad[6];
} uhci_qh_t;

typedef struct {
    uint16_t iobase;
    uint32_t *frame_list;
    uint64_t frame_list_phys;
    uhci_qh_t *qh;
    uint64_t qh_phys;
    uhci_td_t *tds;
    uint64_t tds_phys;
    int td_free;
    uint8_t td_used[UHCI_NUM_TDS];
    usb_hc_t hc;
} uhci_t;

static uhci_t g_uhci[2];
static int g_nuhci;

static inline void uhci_write16(uhci_t *u, uint16_t reg, uint16_t v) { outw(u->iobase + reg, v); }
static inline uint16_t uhci_read16(uhci_t *u, uint16_t reg) { return inw(u->iobase + reg); }
static inline void uhci_write32(uhci_t *u, uint16_t reg, uint32_t v) { outl(u->iobase + reg, v); }

static int uhci_alloc_td(uhci_t *u) {
    for (int i = 0; i < UHCI_NUM_TDS; i++) {
        int idx = (u->td_free + i) % UHCI_NUM_TDS;
        if (!u->td_used[idx]) {
            u->td_used[idx] = 1;
            u->td_free = (idx + 1) % UHCI_NUM_TDS;
            memset(&u->tds[idx], 0, sizeof(uhci_td_t));
            u->tds[idx].link = TD_LINK_TERMINATE;
            return idx;
        }
    }
    return -1;
}

static void uhci_free_td_chain(uhci_t *u, int first, int count) {
    for (int i = 0; i < count; i++) {
        int idx = (first + i) % UHCI_NUM_TDS;
        u->td_used[idx] = 0;
    }
}

static int uhci_reset_port(usb_hc_t *hc, int port) {
    uhci_t *u = (uhci_t *) hc->priv;
    uint16_t reg = (uint16_t) (UHCI_PORTSC1 + (port - 1) * 2);
    uint16_t sc = uhci_read16(u, reg);
    if (!(sc & UHCI_PORT_CCS)) return -1;
    int low_speed = (sc & UHCI_PORT_LSDA) != 0;

    uhci_write16(u, reg, (uint16_t) (sc | UHCI_PORT_PR));
    usb_msleep(50);
    uhci_write16(u, reg, (uint16_t) (uhci_read16(u, reg) & ~UHCI_PORT_PR));
    usb_msleep(10);
    uhci_write16(u, reg, (uint16_t) (uhci_read16(u, reg) | UHCI_PORT_PED));
    usb_msleep(10);

    sc = uhci_read16(u, reg);
    if (!(sc & UHCI_PORT_CCS)) return -1;
    uhci_write16(u, reg, (uint16_t) (sc | UHCI_PORT_CSC | UHCI_PORT_PEDC));
    return low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int uhci_run_tds(uhci_t *u, int first_td, int ntds, int *actual) {
    u->qh->element = u->tds_phys + (uint32_t) first_td * sizeof(uhci_td_t);
    __asm__ volatile("" ::: "memory");

    int result = 0;
    uint32_t timeout = 50000000u;
    while (timeout--) {
        bool done = true;
        for (int i = 0; i < ntds; i++) {
            int idx = (first_td + i) % UHCI_NUM_TDS;
            uint32_t ctrl = u->tds[idx].ctrl;
            if (ctrl & TD_CTRL_ACTIVE) {
                done = false;
                break;
            }
            if (ctrl & (TD_CTRL_STALLED | TD_CTRL_DATABUFFER | TD_CTRL_BABBLE |
                        TD_CTRL_CRCTO | TD_CTRL_BITSTUFF)) {
                result = (ctrl & TD_CTRL_STALLED) ? USB_STALL : USB_BABBLE;
                done = true;
                break;
            }
        }
        if (done) break;
        cpu_relax();
    }
    if (!timeout) result = USB_TIMEOUT;

    u->qh->element = TD_LINK_TERMINATE;
    if (result == 0 && actual) {
        for (int i = ntds - 1; i >= 0; i--) {
            int idx = (first_td + i) % UHCI_NUM_TDS;
            uint32_t alen = (u->tds[idx].ctrl & TD_CTRL_ACTLEN_MASK);
            if (alen != TD_CTRL_ACTLEN_MASK) {
                *actual = (int) alen + 1;
                break;
            }
        }
    }
    return result;
}

static int uhci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_pkt_t *setup, void *buf,
                        int len, int *actual) {
    uhci_t *u = (uhci_t *) hc->priv;
    int low = dev->speed == USB_SPEED_LOW;
    int mps = dev->max_packet0;
    if (mps <= 0) mps = 8;

    int ndata = len > 0 ? (len + mps - 1) / mps : 0;
    int ntds = 1 + ndata + 1;
    if (ntds > 64) return -1;

    int first = uhci_alloc_td(u);
    if (first < 0) return -1;
    for (int i = 1; i < ntds; i++)
        if (uhci_alloc_td(u) < 0) {
            uhci_free_td_chain(u, first, i);
            return -1;
        }

    int idx = first;
    u->tds[idx].ctrl = TD_CTRL_ACTIVE | (3u << TD_CTRL_ERR_SHIFT) | (low ? TD_CTRL_LS : 0);
    u->tds[idx].token = (USB_PID_SETUP << TD_TOKEN_PID_SHIFT) |
                        ((uint32_t) dev->addr << TD_TOKEN_DEVADDR_SHIFT) | (0u << TD_TOKEN_DT_SHIFT) |
                        ((uint32_t) (sizeof(usb_setup_pkt_t) - 1) << TD_TOKEN_MAXLEN_SHIFT);
    u->tds[idx].buffer = (uint32_t) virt_to_phys(setup);

    int toggle = 1;
    uint8_t *data = (uint8_t *) buf;
    uint8_t data_pid = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? USB_PID_IN : USB_PID_OUT;
    for (int i = 0; i < ndata; i++) {
        idx = (first + 1 + i) % UHCI_NUM_TDS;
        int chunk = len - i * mps;
        if (chunk > mps) chunk = mps;
        u->tds[idx].ctrl = TD_CTRL_ACTIVE | (3u << TD_CTRL_ERR_SHIFT) | (low ? TD_CTRL_LS : 0);
        u->tds[idx].token = ((uint32_t) data_pid << TD_TOKEN_PID_SHIFT) |
                            ((uint32_t) dev->addr << TD_TOKEN_DEVADDR_SHIFT) |
                            ((uint32_t) toggle << TD_TOKEN_DT_SHIFT) |
                            ((uint32_t) (chunk - 1) << TD_TOKEN_MAXLEN_SHIFT);
        u->tds[idx].buffer = (uint32_t) virt_to_phys(data + i * mps);
        toggle ^= 1;
    }

    idx = (first + 1 + ndata) % UHCI_NUM_TDS;
    uint8_t status_pid = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? USB_PID_OUT : USB_PID_IN;
    u->tds[idx].ctrl = TD_CTRL_ACTIVE | (3u << TD_CTRL_ERR_SHIFT) | (low ? TD_CTRL_LS : 0);
    u->tds[idx].token = ((uint32_t) status_pid << TD_TOKEN_PID_SHIFT) |
                        ((uint32_t) dev->addr << TD_TOKEN_DEVADDR_SHIFT) |
                        (1u << TD_TOKEN_DT_SHIFT) | (0x7FFu << TD_TOKEN_MAXLEN_SHIFT);
    u->tds[idx].buffer = 0;

    for (int i = 0; i < ntds; i++) {
        int cur = (first + i) % UHCI_NUM_TDS;
        if (i + 1 < ntds) {
            int nxt = (first + i + 1) % UHCI_NUM_TDS;
            u->tds[cur].link = u->tds_phys + (uint32_t) nxt * sizeof(uhci_td_t);
        } else {
            u->tds[cur].link = TD_LINK_TERMINATE;
        }
    }

    int r = uhci_run_tds(u, first, ntds, actual);
    uhci_free_td_chain(u, first, ntds);
    return r;
}

static int uhci_bulk_int(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                         void *buf, int len, int *actual, int is_int) {
    uhci_t *u = (uhci_t *) hc->priv;
    int low = dev->speed == USB_SPEED_LOW;
    int mps = 64;
    for (int i = 0; i < dev->num_ifaces; i++)
        for (int e = 0; e < dev->ifaces[i].num_eps; e++)
            if (dev->ifaces[i].eps[e].addr == ep_addr) mps = dev->ifaces[i].eps[e].max_packet;
    if (mps <= 0) mps = 64;

    int ntds = (len + mps - 1) / mps;
    if (ntds < 1) ntds = 1;
    if (ntds > 128) ntds = 128;

    int first = uhci_alloc_td(u);
    if (first < 0) return -1;
    for (int i = 1; i < ntds; i++)
        if (uhci_alloc_td(u) < 0) {
            uhci_free_td_chain(u, first, i);
            return -1;
        }

    uint8_t pid = (ep_addr & 0x80) ? USB_PID_IN : USB_PID_OUT;
    uint8_t ep = ep_addr & 0x0F;
    uint8_t *data = (uint8_t *) buf;
    int tog = *toggle;

    for (int i = 0; i < ntds; i++) {
        int idx = (first + i) % UHCI_NUM_TDS;
        int chunk = len - i * mps;
        if (chunk > mps) chunk = mps;
        if (chunk < 0) chunk = 0;
        u->tds[idx].ctrl = TD_CTRL_ACTIVE | (3u << TD_CTRL_ERR_SHIFT) | (low ? TD_CTRL_LS : 0) |
                           TD_CTRL_IOC;
        u->tds[idx].token = ((uint32_t) pid << TD_TOKEN_PID_SHIFT) |
                            ((uint32_t) dev->addr << TD_TOKEN_DEVADDR_SHIFT) |
                            ((uint32_t) ep << TD_TOKEN_ENDPT_SHIFT) |
                            ((uint32_t) tog << TD_TOKEN_DT_SHIFT) |
                            ((uint32_t) (chunk - 1) << TD_TOKEN_MAXLEN_SHIFT);
        u->tds[idx].buffer = chunk > 0 ? (uint32_t) virt_to_phys(data + i * mps) : 0;
        tog ^= 1;
        if (i + 1 < ntds) {
            int nxt = (first + i + 1) % UHCI_NUM_TDS;
            u->tds[idx].link = u->tds_phys + (uint32_t) nxt * sizeof(uhci_td_t);
        } else {
            u->tds[idx].link = TD_LINK_TERMINATE;
        }
    }

    int r = uhci_run_tds(u, first, ntds, actual);
    if (r == 0) *toggle = (uint8_t) tog;
    uhci_free_td_chain(u, first, ntds);
    return r;
}

static int uhci_bulk(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                     void *buf, int len, int *actual) {
    return uhci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 0);
}

static int uhci_interrupt(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                          void *buf, int len, int *actual) {
    return uhci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 1);
}

static usb_hc_ops_t g_uhci_ops = {
    .reset_port = uhci_reset_port,
    .control = uhci_control,
    .bulk = uhci_bulk,
    .interrupt = uhci_interrupt,
};

static int uhci_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nuhci >= 2) return 0;
    uint32_t bar4 = pci_read32(bus, dev, fn, 0x20);
    if (!(bar4 & 1) || (bar4 & ~1u) == 0) return 0;

    uhci_t *u = &g_uhci[g_nuhci];
    memset(u, 0, sizeof(*u));
    u->iobase = (uint16_t) (bar4 & 0xFFFCu);
    u->hc.priv = u;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x05u);

    uhci_write16(u, UHCI_USBCMD, UHCI_CMD_HCRESET);
    usb_msleep(20);
    uhci_write16(u, UHCI_USBCMD, 0);
    usb_msleep(5);
    if (uhci_read16(u, UHCI_USBCMD) & UHCI_CMD_HCRESET) return 0;

    uhci_write16(u, UHCI_USBINTR, 0);
    uhci_write16(u, UHCI_FRNUM, 0);

    uint64_t mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return 0;
    u->frame_list_phys = mem;
    u->frame_list = (uint32_t *) phys_to_virt(mem);

    uint64_t mem2 = (uint64_t) pmm_alloc_zeroed();
    if (!mem2) return 0;
    u->qh_phys = mem2;
    u->qh = (uhci_qh_t *) phys_to_virt(mem2);
    u->qh->head = TD_LINK_TERMINATE;
    u->qh->element = TD_LINK_TERMINATE;

    u->tds_phys = (uint64_t) pmm_alloc_contiguous(
        (UHCI_NUM_TDS * sizeof(uhci_td_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!u->tds_phys) return 0;
    u->tds = (uhci_td_t *) phys_to_virt(u->tds_phys);
    memset(u->tds, 0, UHCI_NUM_TDS * sizeof(uhci_td_t));

    for (int i = 0; i < UHCI_FRAME_COUNT; i++)
        u->frame_list[i] = (uint32_t) (u->qh_phys | TD_LINK_QH);

    uhci_write32(u, UHCI_FLBASEADD, (uint32_t) u->frame_list_phys);
    uhci_write16(u, UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_MAXP);
    usb_msleep(5);
    if (uhci_read16(u, UHCI_USBSTS) & UHCI_STS_HCHALTED) {
        log_warn("UHCI: controller halted after start");
        return 0;
    }

    u->hc.name = "uhci";
    u->hc.ops = &g_uhci_ops;
    u->hc.num_ports = 2;
    usb_hc_register(&u->hc);
    g_nuhci++;
    return 1;
}

void uhci_init(void) {
    g_nuhci = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x00)
            uhci_probe_one(d->bus, d->dev, d->fn);
    }
    if (g_nuhci) log_info("UHCI: %d controller(s) initialized", g_nuhci);
}
