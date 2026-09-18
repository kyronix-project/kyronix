#include "usb.h"
#include "../../arch/x86_64/cpu.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci.h"

#define OHCI_REVISION 0x00
#define OHCI_CONTROL 0x04
#define OHCI_CMDSTATUS 0x08
#define OHCI_INTRSTATUS 0x0C
#define OHCI_INTRENABLE 0x10
#define OHCI_INTRDISABLE 0x14
#define OHCI_HCCA 0x18
#define OHCI_PERIODCURRENTED 0x1C
#define OHCI_CONTROLHEADED 0x20
#define OHCI_CONTROLCURRENTED 0x24
#define OHCI_BULKHEADED 0x28
#define OHCI_BULKCURRENTED 0x2C
#define OHCI_DONEHEAD 0x30
#define OHCI_FMINTERVAL 0x34
#define OHCI_FMREMAINING 0x38
#define OHCI_FMNUMBER 0x3C
#define OHCI_PERIODICSTART 0x40
#define OHCI_LSTHRESHOLD 0x44
#define OHCI_RHDESCRIPTORA 0x48
#define OHCI_RHDESCRIPTORB 0x4C
#define OHCI_RHSTATUS 0x50
#define OHCI_RHPORTSTATUS 0x54

#define OHCI_CTRL_CBSR_MASK 0x03
#define OHCI_CTRL_PLE (1u << 2)
#define OHCI_CTRL_IE (1u << 3)
#define OHCI_CTRL_CLE (1u << 4)
#define OHCI_CTRL_BLE (1u << 5)
#define OHCI_CTRL_HCFS_SHIFT 6
#define OHCI_CTRL_HCFS_OPERATIONAL (2u << 6)
#define OHCI_CTRL_IR (1u << 8)
#define OHCI_CTRL_RWC (1u << 9)
#define OHCI_CTRL_RWE (1u << 10)

#define OHCI_CMD_HCR (1u << 0)
#define OHCI_CMD_CLF (1u << 1)
#define OHCI_CMD_BLF (1u << 2)

#define OHCI_INTR_SO (1u << 0)
#define OHCI_INTR_WDH (1u << 1)
#define OHCI_INTR_SF (1u << 2)
#define OHCI_INTR_RD (1u << 3)
#define OHCI_INTR_UE (1u << 4)
#define OHCI_INTR_FNO (1u << 5)
#define OHCI_INTR_RHSC (1u << 6)
#define OHCI_INTR_OC (1u << 30)
#define OHCI_INTR_MIE (1u << 31)

#define OHCI_RHSTATUS_LPS (1u << 0)
#define OHCI_RHSTATUS_OCI (1u << 1)
#define OHCI_RHSTATUS_DRWE (1u << 15)
#define OHCI_RHSTATUS_LPSC (1u << 16)
#define OHCI_RHSTATUS_OCIC (1u << 17)
#define OHCI_RHSTATUS_CRWE (1u << 31)

#define OHCI_PORT_CCS (1u << 0)
#define OHCI_PORT_PES (1u << 1)
#define OHCI_PORT_PSS (1u << 2)
#define OHCI_PORT_POCI (1u << 3)
#define OHCI_PORT_PRS (1u << 4)
#define OHCI_PORT_PPS (1u << 8)
#define OHCI_PORT_LSDA (1u << 9)
#define OHCI_PORT_CSC (1u << 16)
#define OHCI_PORT_PESC (1u << 17)
#define OHCI_PORT_PSSC (1u << 18)
#define OHCI_PORT_OCIC (1u << 19)
#define OHCI_PORT_PRSC (1u << 20)

#define ED_FA_SHIFT 0
#define ED_EN_SHIFT 7
#define ED_DIR_SHIFT 11
#define ED_SPEED (1u << 13)
#define ED_SKIP (1u << 14)
#define ED_FORMAT (1u << 15)
#define ED_MPS_SHIFT 16

#define ED_DIR_TD 0
#define ED_DIR_OUT 1
#define ED_DIR_IN 2

#define TD_DP_SETUP 0
#define TD_DP_OUT 1
#define TD_DP_IN 2

#define TD_CC_NOERROR 0x0
#define TD_CC_CRC 0x1
#define TD_CC_BITSTUFF 0x2
#define TD_CC_TOGGLE 0x3
#define TD_CC_STALL 0x4
#define TD_CC_DEVNOTRESP 0x5
#define TD_CC_PIDCHECKFAIL 0x6
#define TD_CC_UNEXPID 0x7
#define TD_CC_DATAOVERRUN 0x8
#define TD_CC_DATAUNDERRUN 0x9
#define TD_CC_BUFOVERRUN 0xC
#define TD_CC_BUFUNDERRUN 0xD
#define TD_CC_NOTACCESSED 0xF

#define TD_INFO_CC_SHIFT 28
#define TD_INFO_EC_SHIFT 26
#define TD_INFO_T_SHIFT 24
#define TD_INFO_DI_SHIFT 21
#define TD_INFO_DP_SHIFT 19
#define TD_INFO_R (1u << 18)

#define OHCI_NUM_EDS 64
#define OHCI_NUM_TDS 256
#define OHCI_MMIO_VBASE 0xffff931000000000ULL

typedef struct PACKED {
    uint32_t info;
    uint32_t tailp;
    uint32_t headp;
    uint32_t nexted;
    uint32_t pad[4];
} ohci_ed_t;

typedef struct PACKED {
    uint32_t info;
    uint32_t cbp;
    uint32_t nexttd;
    uint32_t be;
    uint32_t pad[4];
} ohci_td_t;

typedef struct PACKED {
    uint32_t inttable[32];
    uint16_t framenumber;
    uint16_t pad1;
    uint32_t donehead;
    uint8_t reserved[116];
} ohci_hcca_t;

typedef struct {
    volatile uint32_t *regs;
    ohci_hcca_t *hcca;
    uint64_t hcca_phys;
    ohci_ed_t *eds;
    uint64_t eds_phys;
    ohci_td_t *tds;
    uint64_t tds_phys;
    uint8_t ed_used[OHCI_NUM_EDS];
    uint8_t td_used[OHCI_NUM_TDS];
    int td_free;
    usb_hc_t hc;
    int num_ports;
} ohci_t;

static ohci_t g_ohci[2];
static int g_nohci;

static inline uint32_t ohci_read(ohci_t *o, uint32_t reg) { return o->regs[reg / 4]; }
static inline void ohci_write(ohci_t *o, uint32_t reg, uint32_t v) { o->regs[reg / 4] = v; }

static int ohci_alloc_ed(ohci_t *o) {
    for (int i = 0; i < OHCI_NUM_EDS; i++) {
        if (!o->ed_used[i]) {
            o->ed_used[i] = 1;
            memset(&o->eds[i], 0, sizeof(ohci_ed_t));
            return i;
        }
    }
    return -1;
}

static void ohci_free_ed(ohci_t *o, int idx) { o->ed_used[idx] = 0; }

static int ohci_alloc_td(ohci_t *o) {
    for (int i = 0; i < OHCI_NUM_TDS; i++) {
        int idx = (o->td_free + i) % OHCI_NUM_TDS;
        if (!o->td_used[idx]) {
            o->td_used[idx] = 1;
            o->td_free = (idx + 1) % OHCI_NUM_TDS;
            memset(&o->tds[idx], 0, sizeof(ohci_td_t));
            return idx;
        }
    }
    return -1;
}

static void ohci_free_td_chain(ohci_t *o, int first, int count) {
    for (int i = 0; i < count; i++) {
        int idx = (first + i) % OHCI_NUM_TDS;
        o->td_used[idx] = 0;
    }
}

static int ohci_reset_port(usb_hc_t *hc, int port) {
    ohci_t *o = (ohci_t *) hc->priv;
    uint32_t reg = OHCI_RHPORTSTATUS + (uint32_t) (port - 1) * 4;
    uint32_t sc = ohci_read(o, reg);
    if (!(sc & OHCI_PORT_CCS)) return -1;
    int low_speed = (sc & OHCI_PORT_LSDA) != 0;

    ohci_write(o, reg, OHCI_PORT_PRS);
    usb_msleep(50);
    while (ohci_read(o, reg) & OHCI_PORT_PRS) cpu_relax();
    ohci_write(o, reg, OHCI_PORT_PRSC);
    usb_msleep(10);

    sc = ohci_read(o, reg);
    if (!(sc & OHCI_PORT_CCS) || !(sc & OHCI_PORT_PES)) return -1;
    ohci_write(o, reg, OHCI_PORT_CSC | OHCI_PORT_PESC | OHCI_PORT_PRSC);
    return low_speed ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int ohci_submit_and_wait(ohci_t *o, int ed_idx, int first_td, int ntds, uint32_t list_reg,
                                uint32_t cmd_bit, int *actual) {
    ohci_ed_t *ed = &o->eds[ed_idx];
    ohci_td_t *last_td = &o->tds[(first_td + ntds - 1) % OHCI_NUM_TDS];
    ed->tailp = o->tds_phys + (uint32_t) ((first_td + ntds) % OHCI_NUM_TDS) * sizeof(ohci_td_t);
    ed->headp = o->tds_phys + (uint32_t) first_td * sizeof(ohci_td_t);
    ed->info &= ~ED_SKIP;

    ohci_write(o, list_reg, o->eds_phys + (uint32_t) ed_idx * sizeof(ohci_ed_t));
    ohci_write(o, OHCI_CMDSTATUS, ohci_read(o, OHCI_CMDSTATUS) | cmd_bit);

    int result = 0;
    uint32_t timeout = 50000000u;
    while (timeout--) {
        uint32_t headp = ed->headp & ~0xFu;
        if (headp == (ed->tailp & ~0xFu)) {
            uint32_t cc = (last_td->info >> TD_INFO_CC_SHIFT) & 0xF;
            if (cc == TD_CC_NOERROR) {
                result = 0;
            } else if (cc == TD_CC_STALL) {
                result = USB_STALL;
            } else if (cc == TD_CC_NOTACCESSED) {
                result = USB_TIMEOUT;
            } else {
                result = USB_BABBLE;
            }
            if (actual && result == 0) {
                uint32_t be = last_td->be;
                *actual = (int) (be ? (be - (last_td->cbp ? last_td->cbp : be + 1)) + 1 : 0);
            }
            break;
        }
        cpu_relax();
    }
    if (!timeout) result = USB_TIMEOUT;

    ohci_write(o, list_reg, 0);
    ed->info |= ED_SKIP;
    return result;
}

static int ohci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_pkt_t *setup, void *buf,
                        int len, int *actual) {
    ohci_t *o = (ohci_t *) hc->priv;
    int low = dev->speed == USB_SPEED_LOW;
    int mps = dev->max_packet0;
    if (mps <= 0) mps = 8;

    int ed_idx = ohci_alloc_ed(o);
    if (ed_idx < 0) return -1;
    ohci_ed_t *ed = &o->eds[ed_idx];
    ed->info = ((uint32_t) dev->addr << ED_FA_SHIFT) | (0u << ED_EN_SHIFT) |
               ((uint32_t) ED_DIR_TD << ED_DIR_SHIFT) | (low ? ED_SPEED : 0) |
               ((uint32_t) mps << ED_MPS_SHIFT);
    ed->nexted = 0;

    int ntds = 3;
    int first = ohci_alloc_td(o);
    if (first < 0) {
        ohci_free_ed(o, ed_idx);
        return -1;
    }
    for (int i = 1; i < ntds; i++) {
        if (ohci_alloc_td(o) < 0) {
            ohci_free_td_chain(o, first, i);
            ohci_free_ed(o, ed_idx);
            return -1;
        }
    }

    ohci_td_t *td0 = &o->tds[first];
    td0->info = (TD_CC_NOTACCESSED << TD_INFO_CC_SHIFT) | (TD_DP_SETUP << TD_INFO_DP_SHIFT) |
                TD_INFO_R;
    td0->cbp = (uint32_t) virt_to_phys(setup);
    td0->be = td0->cbp + sizeof(usb_setup_pkt_t) - 1;

    ohci_td_t *td1 = &o->tds[(first + 1) % OHCI_NUM_TDS];
    uint32_t dp = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? TD_DP_IN : TD_DP_OUT;
    td1->info = (TD_CC_NOTACCESSED << TD_INFO_CC_SHIFT) | (dp << TD_INFO_DP_SHIFT) |
                (1u << TD_INFO_T_SHIFT) | TD_INFO_R;
    if (len > 0) {
        td1->cbp = (uint32_t) virt_to_phys(buf);
        td1->be = td1->cbp + (uint32_t) len - 1;
    } else {
        td1->cbp = 0;
        td1->be = 0;
    }

    ohci_td_t *td2 = &o->tds[(first + 2) % OHCI_NUM_TDS];
    uint32_t status_dp = (setup->bmRequestType & USB_REQTYPE_DIR_IN) ? TD_DP_OUT : TD_DP_IN;
    td2->info = (TD_CC_NOTACCESSED << TD_INFO_CC_SHIFT) | (status_dp << TD_INFO_DP_SHIFT) |
                (1u << TD_INFO_T_SHIFT) | TD_INFO_R;
    td2->cbp = 0;
    td2->be = 0;

    td0->nexttd = o->tds_phys + (uint32_t) ((first + 1) % OHCI_NUM_TDS) * sizeof(ohci_td_t);
    td1->nexttd = o->tds_phys + (uint32_t) ((first + 2) % OHCI_NUM_TDS) * sizeof(ohci_td_t);
    td2->nexttd = 0;

    int r = ohci_submit_and_wait(o, ed_idx, first, ntds, OHCI_CONTROLHEADED, OHCI_CMD_CLF, actual);
    ohci_free_td_chain(o, first, ntds);
    ohci_free_ed(o, ed_idx);
    return r;
}

static int ohci_bulk_int(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                         void *buf, int len, int *actual, int is_int) {
    ohci_t *o = (ohci_t *) hc->priv;
    int low = dev->speed == USB_SPEED_LOW;
    int mps = 64;
    for (int i = 0; i < dev->num_ifaces; i++)
        for (int e = 0; e < dev->ifaces[i].num_eps; e++)
            if (dev->ifaces[i].eps[e].addr == ep_addr) mps = dev->ifaces[i].eps[e].max_packet;
    if (mps <= 0) mps = 64;

    int ed_idx = ohci_alloc_ed(o);
    if (ed_idx < 0) return -1;
    ohci_ed_t *ed = &o->eds[ed_idx];
    uint32_t dir = (ep_addr & 0x80) ? ED_DIR_IN : ED_DIR_OUT;
    ed->info = ((uint32_t) dev->addr << ED_FA_SHIFT) |
               ((uint32_t) (ep_addr & 0x0F) << ED_EN_SHIFT) | (dir << ED_DIR_SHIFT) |
               (low ? ED_SPEED : 0) | ((uint32_t) mps << ED_MPS_SHIFT);
    ed->nexted = 0;

    int ntds = (len + mps - 1) / mps;
    if (ntds < 1) ntds = 1;
    if (ntds > 64) ntds = 64;

    int first = ohci_alloc_td(o);
    if (first < 0) {
        ohci_free_ed(o, ed_idx);
        return -1;
    }
    for (int i = 1; i < ntds; i++) {
        if (ohci_alloc_td(o) < 0) {
            ohci_free_td_chain(o, first, i);
            ohci_free_ed(o, ed_idx);
            return -1;
        }
    }

    uint8_t *data = (uint8_t *) buf;
    int tog = *toggle;
    for (int i = 0; i < ntds; i++) {
        int idx = (first + i) % OHCI_NUM_TDS;
        ohci_td_t *td = &o->tds[idx];
        int chunk = len - i * mps;
        if (chunk > mps) chunk = mps;
        td->info = (TD_CC_NOTACCESSED << TD_INFO_CC_SHIFT) |
                   ((uint32_t) ((ep_addr & 0x80) ? TD_DP_IN : TD_DP_OUT) << TD_INFO_DP_SHIFT) |
                   ((uint32_t) tog << TD_INFO_T_SHIFT) | TD_INFO_R;
        if (chunk > 0) {
            td->cbp = (uint32_t) virt_to_phys(data + i * mps);
            td->be = td->cbp + (uint32_t) chunk - 1;
        } else {
            td->cbp = 0;
            td->be = 0;
        }
        if (i + 1 < ntds) {
            int nxt = (first + i + 1) % OHCI_NUM_TDS;
            td->nexttd = o->tds_phys + (uint32_t) nxt * sizeof(ohci_td_t);
        } else {
            td->nexttd = 0;
        }
        tog ^= 1;
    }

    uint32_t list_reg = OHCI_BULKHEADED;
    uint32_t cmd_bit = OHCI_CMD_BLF;
    int r = ohci_submit_and_wait(o, ed_idx, first, ntds, list_reg, cmd_bit, actual);
    if (r == 0) *toggle = (uint8_t) tog;
    ohci_free_td_chain(o, first, ntds);
    ohci_free_ed(o, ed_idx);
    return r;
}

static int ohci_bulk(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                     void *buf, int len, int *actual) {
    return ohci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 0);
}

static int ohci_interrupt(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                          void *buf, int len, int *actual) {
    return ohci_bulk_int(hc, dev, ep_addr, toggle, buf, len, actual, 1);
}

static usb_hc_ops_t g_ohci_ops = {
    .reset_port = ohci_reset_port,
    .control = ohci_control,
    .bulk = ohci_bulk,
    .interrupt = ohci_interrupt,
};

static int ohci_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nohci >= 2) return 0;
    uint64_t bar0 = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->bus == bus && d->dev == dev && d->fn == fn) {
            bar0 = d->bars[0];
            break;
        }
    }
    if (!bar0) return 0;

    ohci_t *o = &g_ohci[g_nohci];
    memset(o, 0, sizeof(*o));
    o->hc.priv = o;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    for (int i = 0; i < 8; i++) {
        vmm_map(&g_kernel_space, OHCI_MMIO_VBASE + (uint64_t) i * PAGE_SIZE,
                bar0 + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    o->regs = (volatile uint32_t *) (OHCI_MMIO_VBASE + (bar0 & (PAGE_SIZE - 1)));

    ohci_write(o, OHCI_CMDSTATUS, OHCI_CMD_HCR);
    usb_msleep(20);
    if (ohci_read(o, OHCI_CMDSTATUS) & OHCI_CMD_HCR) return 0;

    uint32_t fminterval = ohci_read(o, OHCI_FMINTERVAL);
    uint32_t fit = (fminterval >> 31) & 1;
    uint32_t fsmps = ((fminterval & 0x7FFFu) - 210u) * 6u / 7u;
    ohci_write(o, OHCI_FMINTERVAL, (fsmps << 16) | (fminterval & 0x3FFFu) | (fit << 31));

    uint64_t mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return 0;
    o->hcca_phys = mem;
    o->hcca = (ohci_hcca_t *) phys_to_virt(mem);

    uint64_t mem2 = (uint64_t) pmm_alloc_contiguous(2);
    if (!mem2) return 0;
    o->eds_phys = mem2;
    o->eds = (ohci_ed_t *) phys_to_virt(mem2);
    memset(o->eds, 0, OHCI_NUM_EDS * sizeof(ohci_ed_t));

    o->tds_phys = (uint64_t) pmm_alloc_contiguous(
        (OHCI_NUM_TDS * sizeof(ohci_td_t) + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!o->tds_phys) return 0;
    o->tds = (ohci_td_t *) phys_to_virt(o->tds_phys);
    memset(o->tds, 0, OHCI_NUM_TDS * sizeof(ohci_td_t));

    ohci_write(o, OHCI_HCCA, (uint32_t) o->hcca_phys);
    ohci_write(o, OHCI_CONTROLHEADED, 0);
    ohci_write(o, OHCI_BULKHEADED, 0);
    ohci_write(o, OHCI_INTRDISABLE, 0xFFFFFFFFu);
    ohci_write(o, OHCI_INTRSTATUS, ohci_read(o, OHCI_INTRSTATUS));

    uint32_t ctrl = ohci_read(o, OHCI_CONTROL);
    ctrl &= ~(OHCI_CTRL_CBSR_MASK | OHCI_CTRL_PLE | OHCI_CTRL_IE | OHCI_CTRL_CLE |
              OHCI_CTRL_BLE | OHCI_CTRL_IR | OHCI_CTRL_RWC | OHCI_CTRL_RWE);
    ctrl |= OHCI_CTRL_HCFS_OPERATIONAL | OHCI_CTRL_CLE | OHCI_CTRL_BLE;
    ohci_write(o, OHCI_CONTROL, ctrl);

    ohci_write(o, OHCI_PERIODICSTART, 0x2A2Fu);
    ohci_write(o, OHCI_RHSTATUS, OHCI_RHSTATUS_LPSC);

    uint32_t rhda = ohci_read(o, OHCI_RHDESCRIPTORA);
    o->num_ports = (int) (rhda & 0xFFu);
    if (o->num_ports > 8) o->num_ports = 8;
    usb_msleep(50);

    o->hc.name = "ohci";
    o->hc.ops = &g_ohci_ops;
    o->hc.num_ports = o->num_ports;
    usb_hc_register(&o->hc);
    g_nohci++;
    return 1;
}

void ohci_init(void) {
    g_nohci = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x10)
            ohci_probe_one(d->bus, d->dev, d->fn);
    }
    if (g_nohci) log_info("OHCI: %d controller(s) initialized", g_nohci);
}
