#include "usb.h"
#include "../../arch/x86_64/cpu.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci.h"

#define EHCI_CAPLENGTH 0x00
#define EHCI_HCIVERSION 0x02
#define EHCI_HCSPARAMS 0x04
#define EHCI_HCCPARAMS 0x08
#define EHCI_HCSPPORTROUTE 0x0C

#define EHCI_USBCMD 0x00
#define EHCI_USBSTS 0x04
#define EHCI_USBINTR 0x08
#define EHCI_FRINDEX 0x0C
#define EHCI_CTRLDSSEGMENT 0x10
#define EHCI_PERIODICLISTBASE 0x14
#define EHCI_ASYNCLISTADDR 0x18
#define EHCI_CONFIGFLAG 0x40
#define EHCI_PORTSC 0x44

#define EHCI_CMD_RS (1u << 0)
#define EHCI_CMD_HCRESET (1u << 1)
#define EHCI_CMD_FLS_SHIFT 2
#define EHCI_CMD_PSE (1u << 4)
#define EHCI_CMD_ASE (1u << 5)
#define EHCI_CMD_IAAD (1u << 6)
#define EHCI_CMD_LHCR (1u << 7)
#define EHCI_CMD_ASPMC_SHIFT 8
#define EHCI_CMD_ITC_SHIFT 16

#define EHCI_STS_USBINT (1u << 0)
#define EHCI_STS_USBERRINT (1u << 1)
#define EHCI_STS_PCD (1u << 2)
#define EHCI_STS_FLR (1u << 3)
#define EHCI_STS_HSE (1u << 4)
#define EHCI_STS_IAA (1u << 5)
#define EHCI_STS_HCHALTED (1u << 12)
#define EHCI_STS_RECLAMATION (1u << 13)
#define EHCI_STS_PSS (1u << 14)
#define EHCI_STS_ASS (1u << 15)

#define EHCI_PORT_CCS (1u << 0)
#define EHCI_PORT_CSC (1u << 1)
#define EHCI_PORT_PED (1u << 2)
#define EHCI_PORT_PEDC (1u << 3)
#define EHCI_PORT_OCA (1u << 4)
#define EHCI_PORT_OCC (1u << 5)
#define EHCI_PORT_FPR (1u << 6)
#define EHCI_PORT_SUSPEND (1u << 7)
#define EHCI_PORT_PR (1u << 8)
#define EHCI_PORT_LS_SHIFT 10
#define EHCI_PORT_PP (1u << 12)
#define EHCI_PORT_OWNER (1u << 13)
#define EHCI_PORT_PTC_SHIFT 16

#define QH_LINK_TERMINATE (1u << 0)
#define QH_LINK_TYPE_SHIFT 1
#define QH_LINK_TYPE_QH (1u << 1)
#define QH_LINK_TYPE_ITD (2u << 1)
#define QH_LINK_TYPE_SITD (3u << 1)
#define QH_LINK_TYPE_FSTN (4u << 1)

#define QH_EP_RL_SHIFT 28
#define QH_EP_C (1u << 27)
#define QH_EP_MPL_SHIFT 16
#define QH_EP_H (1u << 15)
#define QH_EP_DTC (1u << 14)
#define QH_EP_EPS_SHIFT 12
#define QH_EP_ENDPT_SHIFT 8
#define QH_EP_I (1u << 7)
#define QH_EP_DEVADDR_SHIFT 0

#define QH_CAP_MULT_SHIFT 30
#define QH_CAP_PORT_SHIFT 23
#define QH_CAP_HUBADDR_SHIFT 16
#define QH_CAP_CMASK_SHIFT 8
#define QH_CAP_SMASK_SHIFT 0

#define QTD_LINK_TERMINATE (1u << 0)
#define QTD_STATUS_ACTIVE (1u << 7)
#define QTD_STATUS_HALTED (1u << 6)
#define QTD_STATUS_DATABUFFER (1u << 5)
#define QTD_STATUS_BABBLE (1u << 4)
#define QTD_STATUS_XACTERR (1u << 3)
#define QTD_STATUS_MISSEDUF (1u << 2)
#define QTD_STATUS_SPLITX (1u << 1)
#define QTD_STATUS_PING (1u << 0)

#define QTD_IOC (1u << 15)
#define QTD_CERR_SHIFT 10
#define QTD_PID_SHIFT 8
#define QTD_PID_OUT 0
#define QTD_PID_IN 1
#define QTD_PID_SETUP 2
#define QTD_DT (1u << 31)
#define QTD_TB_SHIFT 16

#define EHCI_NUM_QHS 32
#define EHCI_NUM_QTDS 256
#define EHCI_MMIO_VBASE 0xffff932000000000ULL

typedef struct PACKED {
    uint32_t link;
    uint32_t alt_link;
    uint32_t status;
    uint32_t buffer[5];
    uint32_t buffer_hi[5];
} ehci_qtd_t;

typedef struct PACKED {
    uint32_t link;
    uint32_t ep_char;
    uint32_t ep_cap;
    uint32_t current_qtd;
    ehci_qtd_t overlay;
    uint32_t pad[3];
} ehci_qh_t;

typedef struct {
    volatile uint32_t *op_regs;
    ehci_qh_t *qhs;
    uint64_t qhs_phys;
    ehci_qtd_t *qtds;
    uint64_t qtds_phys;
    uint8_t qh_used[EHCI_NUM_QHS];
    uint8_t qtd_used[EHCI_NUM_QTDS];
    int qtd_free;
    uint32_t *frame_list;
    uint64_t frame_list_phys;
    usb_hc_t hc;
    int num_ports;
} ehci_t;

static ehci_t g_ehci[2];
static int g_nehci;

static inline uint32_t ehci_read(ehci_t *e, uint32_t reg) { return e->op_regs[reg / 4]; }
static inline void ehci_write(ehci_t *e, uint32_t reg, uint32_t v) { e->op_regs[reg / 4] = v; }

static int ehci_alloc_qh(ehci_t *e) {
    for (int i = 0; i < EHCI_NUM_QHS; i++) {
        if (!e->qh_used[i]) {
            e->qh_used[i] = 1;
            memset(&e->qhs[i], 0, sizeof(ehci_qh_t));
            e->qhs[i].link = QH_LINK_TERMINATE;
            e->qhs[i].overlay.link = QH_LINK_TERMINATE;
            e->qhs[i].overlay.alt_link = QH_LINK_TERMINATE;
            return i;
        }
    }
    return -1;
}

static void ehci_free_qh(ehci_t *e, int idx) { e->qh_used[idx] = 0; }

static int ehci_alloc_qtd(ehci_t *e) {
    for (int i = 0; i < EHCI_NUM_QTDS; i++) {
        int idx = (e->qtd_free + i) % EHCI_NUM_QTDS;
        if (!e->qtd_used[idx]) {
            e->qtd_used[idx] = 1;
            e->qtd_free = (idx + 1) % EHCI_NUM_QTDS;
            memset(&e->qtds[idx], 0, sizeof(ehci_qtd_t));
            e->qtds[idx].link = QH_LINK_TERMINATE;
            e->qtds[idx].alt_link = QH_LINK_TERMINATE;
            return idx;
        }
    }
    return -1;
}

static void ehci_free_qtd_chain(ehci_t *e, int first, int count) {
    for (int i = 0; i < count; i++) {
        int idx = (first + i) % EHCI_NUM_QTDS;
        e->qtd_used[idx] = 0;
    }
}

static int ehci_reset_port(usb_hc_t *hc, int port) {
    ehci_t *e = (ehci_t *) hc->priv;
    uint32_t reg = EHCI_PORTSC + (uint32_t) (port - 1) * 4;
    uint32_t sc = ehci_read(e, reg);
    if (!(sc & EHCI_PORT_CCS)) return -1;

    ehci_write(e, reg, sc | EHCI_PORT_PR);
    usb_msleep(50);
    ehci_write(e, reg, (sc & ~EHCI_PORT_PR) | EHCI_PORT_PED);
    usb_msleep(10);

    sc = ehci_read(e, reg);
    if (!(sc & EHCI_PORT_CCS)) return -1;
    ehci_write(e, reg, sc | EHCI_PORT_CSC | EHCI_PORT_PEDC);
    return USB_SPEED_HIGH;
}

static int ehci_submit_and_wait(ehci_t *e, int qh_idx, int first_qtd, int nqtds, int *actual) {
    ehci_qh_t *qh = &e->qhs[qh_idx];
    ehci_qtd_t *last_qtd = &e->qtds[(first_qtd + nqtds - 1) % EHCI_NUM_QTDS];

    qh->overlay.link = e->qtds_phys + (uint32_t) first_qtd * sizeof(ehci_qtd_t);
    qh->overlay.alt_link = QH_LINK_TERMINATE;
    qh->overlay.status = 0;
    qh->ep_char &= ~QH_EP_I;

    ehci_write(e, EHCI_ASYNCLISTADDR, (uint32_t) (e->qhs_phys + (uint32_t) qh_idx * sizeof(ehci_qh_t)));
    ehci_write(e, EHCI_USBCMD, ehci_read(e, EHCI_USBCMD) | EHCI_CMD_ASE | EHCI_CMD_IAAD);

    int result = 0;
    uint32_t timeout = 50000000u;
    while (timeout--) {
        uint32_t sts = last_qtd->status;
        if (!(sts & QTD_STATUS_ACTIVE)) {
            if (sts & QTD_STATUS_HALTED) {
                if (sts & QTD_STATUS_BABBLE) result = USB_BABBLE;
                else if (sts & QTD_STATUS_DATABUFFER) result = USB_STALL;
                else result = USB_STALL;
            } else {
                result = 0;
            }
            if (actual && result == 0) {
                *actual = (int) ((last_qtd->status >> QTD_TB_SHIFT) & 0x7FFFu);
            }
            break;
        }
        cpu_relax();
    }
    if (!timeout) result = USB_TIMEOUT;

    ehci_write(e, EHCI_USBCMD, ehci_read(e, EHCI_USBCMD) & ~EHCI_CMD_ASE);
    qh->ep_char |= QH_EP_I;
    return result;
}

static int ehci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_pkt_t *setup, void *buf,
                        int len, int *actual) {
    ehci_t *e = (ehci_t *) hc->priv;
    int mps = dev->max_packet0;
    if (mps <= 0) mps = 64;

    int qh_idx = ehci_alloc_qh(e);
    if (qh_idx < 0) return -1;
    ehci_qh_t *qh = &e->qhs[qh_idx];
    qh->ep_char = ((uint32_t) mps << QH_EP_MPL_SHIFT) | (2u << QH_EP_EPS_SHIFT) |
                  (0u << QH_EP_ENDPT_SHIFT) | ((uint32_t) dev->addr << QH_EP_DEVADDR_SHIFT) |
                  QH_EP_DTC;
    qh->ep_cap = (1u << QH_CAP_MULT_SHIFT) | (0xFFu << QH_CAP_CMASK_SHIFT) |
                 (0x01u << QH_CAP_SMASK_SHIFT);
    qh->link = (uint32_t) (e->qhs_phys + (uint32_t) qh_idx * sizeof(ehci_qh_t)) |
               QH_LINK_TYPE_QH;

    int ndata = len > 0 ? (len + mps - 1) / mps : 0;
    int nqtds = 1 + ndata + 1;
    if (nqtds > 64) {
        ehci_free_qh(e, qh_idx);
        return -1;
    }

    int first = ehci_alloc_qtd(e);
    if (first < 0) {
        ehci_free_qh(e, qh_idx);
        return -1;
    }
    for (int i = 1; i < nqtds; i++) {
        if (ehci_alloc_qtd(e) < 0) {
            ehci_free_qtd_chain(e, first, i);
            ehci_free_qh(e, qh_idx);
            return -1;
        }
    }

    int idx = first;
    e->qtds[idx].status = QTD_STATUS_ACTIVE | (3u << QTD_CERR_SHIFT);
    e->qtds[idx].buffer[0] = (uint32_t) virt_to_phys(setup);
    e->qtds[idx].status |= (uint32_t) sizeof(usb_setup_pkt_t) << QTD_TB_SHIFT;
    e->qtds[idx].status |= QTD_PID_SETUP << QTD_PID_SHIFT;

    int toggle = 1;
    uint8_t *data = (uint8_t *) buf;
    uint32_t data_pid = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? QTD_PID_IN : QTD_PID_OUT;
    for (int i = 0; i < ndata; i++) {
        idx = (first + 1 + i) % EHCI_NUM_QTDS;
        int chunk = len - i * mps;
        if (chunk > mps) chunk = mps;
        e->qtds[idx].status = QTD_STATUS_ACTIVE | (3u << QTD_CERR_SHIFT) |
                              ((uint32_t) chunk << QTD_TB_SHIFT) |
                              (data_pid << QTD_PID_SHIFT) | (toggle ? QTD_DT : 0);
        e->qtds[idx].buffer[0] = (uint32_t) virt_to_phys(data + i * mps);
        toggle ^= 1;
    }

    idx = (first + 1 + ndata) % EHCI_NUM_QTDS;
    uint32_t status_pid = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? QTD_PID_OUT : QTD_PID_IN;
    e->qtds[idx].status = QTD_STATUS_ACTIVE | (3u << QTD_CERR_SHIFT) | QTD_IOC |
                          (status_pid << QTD_PID_SHIFT) | QTD_DT;
    e->qtds[idx].buffer[0] = 0;

    for (int i = 0; i < nqtds; i++) {
        int cur = (first + i) % EHCI_NUM_QTDS;
        if (i + 1 < nqtds) {
            int nxt = (first + i + 1) % EHCI_NUM_QTDS;
            e->qtds[cur].link = e->qtds_phys + (uint32_t) nxt * sizeof(ehci_qtd_t);
        } else {
            e->qtds[cur].link = QH_LINK_TERMINATE;
        }
    }

    int r = ehci_submit_and_wait(e, qh_idx, first, nqtds, actual);
    ehci_free_qtd_chain(e, first, nqtds);
    ehci_free_qh(e, qh_idx);
    return r;
}

static int ehci_bulk_int(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                         void *buf, int len, int *actual, int is_int) {
    ehci_t *e = (ehci_t *) hc->priv;
    int mps = 512;
    for (int i = 0; i < dev->num_ifaces; i++)
        for (int ep = 0; ep < dev->ifaces[i].num_eps; ep++)
            if (dev->ifaces[i].eps[ep].addr == ep_addr) mps = dev->ifaces[i].eps[ep].max_packet;
    if (mps <= 0) mps = 512;

    int qh_idx = ehci_alloc_qh(e);
    if (qh_idx < 0) return -1;
    ehci_qh_t *qh = &e->qhs[qh_idx];
    qh->ep_char = ((uint32_t) mps << QH_EP_MPL_SHIFT) | (2u << QH_EP_EPS_SHIFT) |
                  ((uint32_t) (ep_addr & 0x0F) << QH_EP_ENDPT_SHIFT) |
                  ((uint32_t) dev->addr << QH_EP_DEVADDR_SHIFT);
    qh->ep_cap = (1u << QH_CAP_MULT_SHIFT) | (0xFFu << QH_CAP_CMASK_SHIFT) |
                 (0x01u << QH_CAP_SMASK_SHIFT);
    qh->link = (uint32_t) (e->qhs_phys + (uint32_t) qh_idx * sizeof(ehci_qh_t)) |
               QH_LINK_TYPE_QH;

    int nqtds = (len + mps - 1) / mps;
    if (nqtds < 1) nqtds = 1;
    if (nqtds > 128) nqtds = 128;

    int first = ehci_alloc_qtd(e);
    if (first < 0) {
        ehci_free_qh(e, qh_idx);
        return -1;
    }
    for (int i = 1; i < nqtds; i++) {
        if (ehci_alloc_qtd(e) < 0) {
            ehci_free_qtd_chain(e, first, i);
            ehci_free_qh(e, qh_idx);
            return -1;
        }
    }

    uint8_t *data = (uint8_t *) buf;
    int tog = *toggle;
    uint32_t pid = (ep_addr & 0x80) ? QTD_PID_IN : QTD_PID_OUT;
    for (int i = 0; i < nqtds; i++) {
        int idx = (first + i) % EHCI_NUM_QTDS;
        int chunk = len - i * mps;
        if (chunk > mps) chunk = mps;
        if (chunk < 0) chunk = 0;
        e->qtds[idx].status = QTD_STATUS_ACTIVE | (3u << QTD_CERR_SHIFT) | QTD_IOC |
                              ((uint32_t) chunk << QTD_TB_SHIFT) | (pid << QTD_PID_SHIFT) |
                              (tog ? QTD_DT : 0);
        e->qtds[idx].buffer[0] = chunk > 0 ? (uint32_t) virt_to_phys(data + i * mps) : 0;
        if (i + 1 < nqtds) {
            int nxt = (first + i + 1) % EHCI_NUM_QTDS;
            e->qtds[idx].link = e->qtds_phys + (uint32_t) nxt * sizeof(ehci_qtd_t);
        } else {
            e->qtds[idx].link = QH_LINK_TERMINATE;
        }
        tog ^= 1;
    }

    int r = ehci_submit_and_wait(e, qh_idx, first, nqtds, actual);
    if (r == 0) *toggle = (uint8_t) tog;
    ehci_free_qtd_chain(e, first, nqtds);
    ehci_free_qh(e, qh_idx);
    return r;
}

static int ehci_bulk(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                     void *buf, int len, int *actual) {
    return ehci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 0);
}

static int ehci_interrupt(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                          void *buf, int len, int *actual) {
    return ehci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 1);
}

static usb_hc_ops_t g_ehci_ops = {
    .reset_port = ehci_reset_port,
    .control = ehci_control,
    .bulk = ehci_bulk,
    .interrupt = ehci_interrupt,
};

static int ehci_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nehci >= 2) return 0;
    uint64_t bar0 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            bar0 = d->bars[0];
            break;
        }
    }
    if (!bar0) return 0;

    ehci_t *e = &g_ehci[g_nehci];
    memset(e, 0, sizeof(*e));
    e->hc.priv = e;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    uint64_t bar0_phys = bar0 & PAGE_MASK;
    for (int i = 0; i < 8; i++) {
        vmm_map(&g_kernel_space, EHCI_MMIO_VBASE + (uint64_t) i * PAGE_SIZE,
                bar0_phys + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    volatile uint8_t *mmio = (volatile uint8_t *) (EHCI_MMIO_VBASE + (bar0 & (PAGE_SIZE - 1)));
    uint8_t caplength = mmio[EHCI_CAPLENGTH];
    uint32_t hcsparams = *(volatile uint32_t *) (mmio + EHCI_HCSPARAMS);
    e->num_ports = (int) (hcsparams & 0x0Fu);
    if (e->num_ports > 8) e->num_ports = 8;
    e->op_regs = (volatile uint32_t *) (mmio + caplength);

    ehci_write(e, EHCI_USBCMD, EHCI_CMD_HCRESET);
    usb_msleep(50);
    if (ehci_read(e, EHCI_USBCMD) & EHCI_CMD_HCRESET) return 0;

    uint64_t mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return 0;
    e->frame_list_phys = mem;
    e->frame_list = (uint32_t *) phys_to_virt(mem);
    for (int i = 0; i < 1024; i++) e->frame_list[i] = QH_LINK_TERMINATE;

    uint64_t mem2 = (uint64_t) pmm_alloc_contiguous(2);
    if (!mem2) return 0;
    e->qhs_phys = mem2;
    e->qhs = (ehci_qh_t *) phys_to_virt(mem2);
    memset(e->qhs, 0, EHCI_NUM_QHS * sizeof(ehci_qh_t));

    e->qtds_phys = (uint64_t) pmm_alloc_contiguous(
        (EHCI_NUM_QTDS * sizeof(ehci_qtd_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!e->qtds_phys) return 0;
    e->qtds = (ehci_qtd_t *) phys_to_virt(e->qtds_phys);
    memset(e->qtds, 0, EHCI_NUM_QTDS * sizeof(ehci_qtd_t));

    ehci_write(e, EHCI_PERIODICLISTBASE, (uint32_t) e->frame_list_phys);
    ehci_write(e, EHCI_CTRLDSSEGMENT, 0);
    ehci_write(e, EHCI_USBINTR, 0);
    ehci_write(e, EHCI_USBSTS, ehci_read(e, EHCI_USBSTS));
    ehci_write(e, EHCI_CONFIGFLAG, 1);

    ehci_write(e, EHCI_USBCMD, (0u << EHCI_CMD_ITC_SHIFT) | (0u << EHCI_CMD_FLS_SHIFT) |
                                   EHCI_CMD_RS);
    usb_msleep(10);
    if (ehci_read(e, EHCI_USBSTS) & EHCI_STS_HCHALTED) {
        log_warn("EHCI: controller halted after start");
        return 0;
    }

    e->hc.name = "ehci";
    e->hc.ops = &g_ehci_ops;
    e->hc.num_ports = e->num_ports;
    usb_hc_register(&e->hc);
    g_nehci++;
    return 1;
}

void ehci_init(void) {
    g_nehci = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x20)
            ehci_probe_one(d->bus, d->dev, d->fn);
    }
    if (g_nehci) log_info("EHCI: %d controller(s) initialized", g_nehci);
}
