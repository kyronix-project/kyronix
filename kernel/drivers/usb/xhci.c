#include "usb.h"
#include "../../arch/x86_64/cpu.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../pci.h"

#define XHCI_CAP_CAPLENGTH 0x00
#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCSPARAMS3 0x0C
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF 0x14
#define XHCI_CAP_RTSOFF 0x18

#define XHCI_OP_USBCMD 0x00
#define XHCI_OP_USBSTS 0x04
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_DNCTRL 0x14
#define XHCI_OP_CRCR 0x18
#define XHCI_OP_DCBAAP 0x30
#define XHCI_OP_CONFIG 0x38
#define XHCI_OP_PORTSC 0x400

#define XHCI_CMD_RUN (1u << 0)
#define XHCI_CMD_HCRST (1u << 1)
#define XHCI_CMD_INTE (1u << 2)
#define XHCI_CMD_HSEE (1u << 3)
#define XHCI_CMD_LHCRST (1u << 7)
#define XHCI_CMD_CSS (1u << 8)
#define XHCI_CMD_CRS (1u << 9)
#define XHCI_CMD_EWE (1u << 10)

#define XHCI_STS_HCH (1u << 0)
#define XHCI_STS_HSE (1u << 2)
#define XHCI_STS_EINT (1u << 3)
#define XHCI_STS_PCD (1u << 4)
#define XHCI_STS_SSS (1u << 8)
#define XHCI_STS_RSS (1u << 9)
#define XHCI_STS_SRE (1u << 10)
#define XHCI_STS_CNR (1u << 11)
#define XHCI_STS_HCE (1u << 12)

#define XHCI_PORTSC_CCS (1u << 0)
#define XHCI_PORTSC_PED (1u << 1)
#define XHCI_PORTSC_OCA (1u << 3)
#define XHCI_PORTSC_PR (1u << 4)
#define XHCI_PORTSC_PLS_SHIFT 5
#define XHCI_PORTSC_PLS_MASK (0xFu << 5)
#define XHCI_PORTSC_PLS_U0 (0u << 5)
#define XHCI_PORTSC_PP (1u << 9)
#define XHCI_PORTSC_SPEED_SHIFT 10
#define XHCI_PORTSC_SPEED_MASK (0xFu << 10)
#define XHCI_PORTSC_PIC_SHIFT 14
#define XHCI_PORTSC_LWS (1u << 16)
#define XHCI_PORTSC_CSC (1u << 17)
#define XHCI_PORTSC_PEC (1u << 18)
#define XHCI_PORTSC_WRC (1u << 19)
#define XHCI_PORTSC_OCC (1u << 20)
#define XHCI_PORTSC_PRC (1u << 21)
#define XHCI_PORTSC_PLC (1u << 22)
#define XHCI_PORTSC_CEC (1u << 23)
#define XHCI_PORTSC_CAS (1u << 24)
#define XHCI_PORTSC_WCE (1u << 25)
#define XHCI_PORTSC_WDE (1u << 26)
#define XHCI_PORTSC_WOE (1u << 27)
#define XHCI_PORTSC_DR (1u << 30)
#define XHCI_PORTSC_WPR (1u << 31)

#define XHCI_PORTSC_CHANGE_BITS                                                     \
    (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC |        \
     XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

#define XHCI_RT_IMAN 0x20
#define XHCI_RT_IMOD 0x24
#define XHCI_RT_ERSTSZ 0x28
#define XHCI_RT_ERSTBA 0x30
#define XHCI_RT_ERDP 0x38

#define XHCI_IMAN_IP (1u << 0)
#define XHCI_IMAN_IE (1u << 1)
#define XHCI_ERDP_EHB (1u << 3)

#define TRB_TYPE_NORMAL 1
#define TRB_TYPE_SETUP 2
#define TRB_TYPE_DATA 3
#define TRB_TYPE_STATUS 4
#define TRB_TYPE_LINK 6
#define TRB_TYPE_EVENT_DATA 8
#define TRB_TYPE_NOOP 9
#define TRB_TYPE_ENABLE_SLOT 10
#define TRB_TYPE_DISABLE_SLOT 11
#define TRB_TYPE_ADDRESS_DEVICE 12
#define TRB_TYPE_CONFIG_EP 13
#define TRB_TYPE_EVALUATE_CONTEXT 14
#define TRB_TYPE_RESET_EP 15
#define TRB_TYPE_STOP_EP 16
#define TRB_TYPE_SET_TR_DEQUEUE 17
#define TRB_TYPE_RESET_DEVICE 18
#define TRB_TYPE_NOOP_CMD 23
#define TRB_TYPE_TRANSFER_EVENT 32
#define TRB_TYPE_CMD_COMPLETION 33
#define TRB_TYPE_PORT_STATUS_CHANGE 34

#define TRB_CYCLE (1u << 0)
#define TRB_TOGGLE_CYCLE (1u << 9)
#define TRB_IOC (1u << 5)
#define TRB_IDT (1u << 6)
#define TRB_ENT (1u << 1)
#define TRB_ISP (1u << 2)
#define TRB_NS (1u << 3)
#define TRB_CH (1u << 4)
#define TRB_BSR (1u << 9)

#define TRB_TYPE_SHIFT 10
#define TRB_DIR_IN (1u << 16)
#define TRB_TRT_SHIFT 16
#define TRB_TRT_NO_DATA 0
#define TRB_TRT_OUT 2
#define TRB_TRT_IN 3
#define TRB_SLOT_SHIFT 24
#define TRB_EP_SHIFT 16
#define TRB_TBC_SHIFT 7

#define COMP_SUCCESS 1
#define COMP_DATA_BUFFER_ERROR 2
#define COMP_BABBLE 3
#define COMP_USB_TRANSACTION_ERROR 4
#define COMP_TRB_ERROR 5
#define COMP_STALL_ERROR 6
#define COMP_RESOURCE_ERROR 7
#define COMP_BANDWIDTH_ERROR 8
#define COMP_NO_SLOTS_ERROR 9
#define COMP_SHORT_PACKET 13
#define COMP_RING_UNDERRUN 14
#define COMP_RING_OVERRUN 15
#define COMP_EVENT_RING_FULL 21
#define COMP_CONTEXT_STATE_ERROR 26

#define EP_TYPE_NOT_VALID 0
#define EP_TYPE_ISOCH_OUT 1
#define EP_TYPE_BULK_OUT 2
#define EP_TYPE_INT_OUT 3
#define EP_TYPE_CONTROL 4
#define EP_TYPE_ISOCH_IN 5
#define EP_TYPE_BULK_IN 6
#define EP_TYPE_INT_IN 7

#define SLOT_CTX_ENTRIES_SHIFT 27
#define SLOT_CTX_SPEED_SHIFT 20
#define SLOT_CTX_ROOT_PORT_SHIFT 16
#define SLOT_CTX_ROUTE_MASK 0xFFFFFu

#define EP_CTX_STATE_SHIFT 0
#define EP_CTX_MPS_SHIFT 16
#define EP_CTX_CERR_SHIFT 1
#define EP_CTX_EP_TYPE_SHIFT 3
#define EP_CTX_MAX_BURST_SHIFT 8
#define EP_CTX_INTERVAL_SHIFT 16
#define EP_CTX_LSA (1u << 15)
#define EP_CTX_MAX_P_STREAMS_SHIFT 10

#define TR_RING_SIZE 64
#define CMD_RING_SIZE 64
#define EVT_RING_SIZE 64
#define XHCI_MAX_SLOTS 32
#define XHCI_MAX_EPS 8
#define XHCI_MMIO_VBASE 0xffff933000000000ULL
#define XHCI_MMIO_MAX_PAGES 64

typedef struct PACKED {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

typedef struct PACKED {
    uint64_t ring_base;
    uint16_t ring_size;
    uint16_t reserved;
} xhci_erst_entry_t;

typedef struct PACKED {
    uint32_t dw[8];
} xhci_slot_ctx_t;

typedef struct PACKED {
    uint32_t dw[8];
} xhci_ep_ctx_t;

typedef struct PACKED {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} xhci_input_ctrl_ctx_t;

typedef struct {
    xhci_input_ctrl_ctx_t ctrl;
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t ep[31];
} xhci_input_ctx_t;

typedef struct {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t ep[31];
} xhci_dev_ctx_t;

typedef struct {
    xhci_trb_t *trbs;
    uint64_t phys;
    uint32_t enq;
    uint32_t deq;
    uint8_t cycle;
} xhci_ring_t;

typedef struct {
    xhci_ring_t rings[XHCI_MAX_EPS];
    bool ring_ok[XHCI_MAX_EPS];
    uint8_t addr;
    uint8_t root_port;
    uint8_t speed;
    int slot_id;
    xhci_input_ctx_t *in_ctx;
    uint64_t in_ctx_phys;
    xhci_dev_ctx_t *dev_ctx;
    uint64_t dev_ctx_phys;
    bool active;
    bool addressed;
} xhci_slot_t;

typedef struct {
    volatile uint8_t *cap;
    volatile uint32_t *op;
    volatile uint32_t *rt;
    volatile uint32_t *db;
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;
    xhci_ring_t cmd_ring;
    xhci_trb_t *evt_ring;
    uint64_t evt_ring_phys;
    xhci_erst_entry_t *erst;
    uint64_t erst_phys;
    uint32_t evt_deq;
    uint8_t evt_cycle;
    xhci_slot_t slots[XHCI_MAX_SLOTS];
    usb_hc_t hc;
    int num_ports;
    int max_slots;
    uint8_t page_shift;
    uint64_t ctx_size;
} xhci_t;

static xhci_t g_xhci;
static int g_nxhci;
static usb_device_t *g_xhci_pending_dev;

static void xhci_ring_init(xhci_ring_t *r, xhci_trb_t *trbs, uint64_t phys) {
    r->trbs = trbs;
    r->phys = phys;
    r->enq = 0;
    r->deq = 0;
    r->cycle = 1;
    memset(trbs, 0, TR_RING_SIZE * sizeof(xhci_trb_t));
    trbs[TR_RING_SIZE - 1].param = phys;
    trbs[TR_RING_SIZE - 1].status = 0;
    trbs[TR_RING_SIZE - 1].control = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE;
}

static void xhci_ring_push(xhci_ring_t *r, const xhci_trb_t *trb) {
    xhci_trb_t *t = &r->trbs[r->enq];
    *t = *trb;
    t->control = (t->control & ~TRB_CYCLE) | r->cycle;
    __asm__ volatile("" ::: "memory");
    r->enq++;
    if (r->enq == TR_RING_SIZE - 1) {
        r->trbs[TR_RING_SIZE - 1].control =
            (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_TOGGLE_CYCLE | r->cycle;
        __asm__ volatile("" ::: "memory");
        r->enq = 0;
        r->cycle ^= 1;
    }
}

static void xhci_doorbell(xhci_t *x, uint32_t slot, uint32_t target) {
    x->db[slot] = target;
    __asm__ volatile("" ::: "memory");
}

static int xhci_wait_event(xhci_t *x, uint8_t expect_type, xhci_trb_t *out, uint32_t timeout_ms) {
    uint32_t spins = timeout_ms * 50000u;
    while (spins--) {
        xhci_trb_t *t = &x->evt_ring[x->evt_deq];
        uint32_t ctrl = t->control;
        if ((ctrl & TRB_CYCLE) == x->evt_cycle) {
            uint8_t type = (uint8_t) ((ctrl >> TRB_TYPE_SHIFT) & 0x3F);
            *out = *t;
            x->evt_deq++;
            if (x->evt_deq == EVT_RING_SIZE) {
                x->evt_deq = 0;
                x->evt_cycle ^= 1;
            }
            uint64_t erdp = x->evt_ring_phys + x->evt_deq * sizeof(xhci_trb_t);
            x->rt[(XHCI_RT_ERDP) / 4] = (uint32_t) (erdp & 0xFFFFFFFFu) | XHCI_ERDP_EHB;
            x->rt[(XHCI_RT_ERDP + 4) / 4] = (uint32_t) (erdp >> 32);
            if (type == expect_type) return 0;
        } else {
            cpu_relax();
        }
    }
    return USB_TIMEOUT;
}

static int xhci_run_cmd(xhci_t *x, xhci_trb_t *cmd, xhci_trb_t *result) {
    xhci_ring_push(&x->cmd_ring, cmd);
    xhci_doorbell(x, 0, 0);
    int r = xhci_wait_event(x, TRB_TYPE_CMD_COMPLETION, result, 500);
    if (r) return r;
    uint32_t cc = (result->status >> 24) & 0xFF;
    if (cc != COMP_SUCCESS) return -(int) cc;
    return 0;
}

static int xhci_enable_slot(xhci_t *x) {
    xhci_trb_t cmd = {0}, res;
    cmd.control = TRB_TYPE_ENABLE_SLOT << TRB_TYPE_SHIFT;
    int r = xhci_run_cmd(x, &cmd, &res);
    if (r) return -1;
    return (int) ((res.control >> TRB_SLOT_SHIFT) & 0xFF);
}

static int xhci_address_device(xhci_t *x, int slot_id, xhci_slot_t *slot, bool bsr) {
    xhci_trb_t cmd = {0}, res;
    cmd.param = slot->in_ctx_phys;
    cmd.control = (TRB_TYPE_ADDRESS_DEVICE << TRB_TYPE_SHIFT) |
                  ((uint32_t) slot_id << TRB_SLOT_SHIFT) | (bsr ? TRB_BSR : 0);
    return xhci_run_cmd(x, &cmd, &res);
}

static int xhci_config_ep(xhci_t *x, int slot_id, xhci_slot_t *slot) {
    xhci_trb_t cmd = {0}, res;
    cmd.param = slot->in_ctx_phys;
    cmd.control = (TRB_TYPE_CONFIG_EP << TRB_TYPE_SHIFT) | ((uint32_t) slot_id << TRB_SLOT_SHIFT);
    return xhci_run_cmd(x, &cmd, &res);
}

static uint32_t xhci_portsc(xhci_t *x, int port) {
    return x->op[(XHCI_OP_PORTSC + (uint32_t) (port - 1) * 0x10) / 4];
}

static void xhci_portsc_write(xhci_t *x, int port, uint32_t v) {
    x->op[(XHCI_OP_PORTSC + (uint32_t) (port - 1) * 0x10) / 4] = v;
}

static int xhci_reset_port(usb_hc_t *hc, int port) {
    xhci_t *x = (xhci_t *) hc->priv;
    uint32_t sc = xhci_portsc(x, port);
    if (!(sc & XHCI_PORTSC_CCS)) return -1;

    xhci_portsc_write(x, port, sc | XHCI_PORTSC_PR);
    usb_msleep(60);
    uint32_t to = 1000;
    while (xhci_portsc(x, port) & XHCI_PORTSC_PR) {
        if (!to--) return -1;
        usb_msleep(1);
    }
    usb_msleep(10);

    sc = xhci_portsc(x, port);
    if (!(sc & XHCI_PORTSC_CCS) || !(sc & XHCI_PORTSC_PED)) return -1;

    int speed = (int) ((sc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT);
    xhci_portsc_write(x, port, (sc & ~(XHCI_PORTSC_PED | XHCI_PORTSC_PR)) |
                                   XHCI_PORTSC_CHANGE_BITS | XHCI_PORTSC_PP);
    return speed;
}

static xhci_slot_t *xhci_find_slot_by_dev(xhci_t *x, usb_device_t *dev) {
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (x->slots[i].active && x->slots[i].addr == dev->addr &&
            x->slots[i].root_port == dev->port)
            return &x->slots[i];
    }
    return NULL;
}

static int xhci_ep_dci(uint8_t ep_addr) {
    int ep = ep_addr & 0x0F;
    if (ep == 0) return 1;
    return ep * 2 + ((ep_addr & 0x80) ? 1 : 0);
}

static int xhci_ep_type(uint8_t ep_addr, uint8_t attr) {
    uint8_t t = attr & 3;
    bool in = (ep_addr & 0x80) != 0;
    if (t == 2) return in ? EP_TYPE_BULK_IN : EP_TYPE_BULK_OUT;
    if (t == 3) return in ? EP_TYPE_INT_IN : EP_TYPE_INT_OUT;
    if (t == 1) return in ? EP_TYPE_ISOCH_IN : EP_TYPE_ISOCH_OUT;
    return EP_TYPE_CONTROL;
}

static int xhci_ensure_ep(xhci_t *x, xhci_slot_t *slot, uint8_t ep_addr, uint8_t attr,
                          uint16_t mps, uint8_t interval) {
    int dci = xhci_ep_dci(ep_addr);
    if (dci < 1 || dci > 31) return -1;
    int ring_idx = dci - 1;
    if (ring_idx >= XHCI_MAX_EPS) return -1;
    if (slot->ring_ok[ring_idx]) return 0;

    uint64_t phys = (uint64_t) pmm_alloc_zeroed();
    if (!phys) return -1;
    xhci_ring_t *ring = &slot->rings[ring_idx];
    xhci_ring_init(ring, (xhci_trb_t *) phys_to_virt(phys), phys);
    ring->trbs[TR_RING_SIZE - 1].control |= ring->cycle;

    memset(slot->in_ctx, 0, x->ctx_size * 32);
    slot->in_ctx->ctrl.add_flags = (1u << dci);
    xhci_ep_ctx_t *ep = &slot->in_ctx->ep[dci - 1];
    int eptype = xhci_ep_type(ep_addr, attr);
    ep->dw[0] = 0;
    ep->dw[1] = (3u << EP_CTX_CERR_SHIFT) | ((uint32_t) eptype << EP_CTX_EP_TYPE_SHIFT) |
                ((uint32_t) mps << EP_CTX_MPS_SHIFT);
    ep->dw[2] = (uint32_t) (ring->phys | 1u);
    ep->dw[3] = (uint32_t) (ring->phys >> 32);
    ep->dw[4] = 8;
    if (eptype == EP_TYPE_INT_IN || eptype == EP_TYPE_INT_OUT) {
        uint8_t ival = interval ? interval : 10;
        uint8_t exp = 3;
        while (exp < 15 && (1u << exp) < ival) exp++;
        ep->dw[0] = (uint32_t) exp;
        ep->dw[4] = (uint32_t) mps;
    }

    int r = xhci_config_ep(x, slot->slot_id, slot);
    if (r) {
        pmm_free((void *) phys);
        return r;
    }
    slot->ring_ok[ring_idx] = true;
    return 0;
}

static int xhci_submit_data(xhci_t *x, xhci_slot_t *slot, uint8_t ep_addr, bool dir_in,
                            uint64_t buf_phys, uint32_t len, int *actual) {
    int dci = xhci_ep_dci(ep_addr);
    int ring_idx = dci - 1;
    if (ring_idx < 0 || ring_idx >= XHCI_MAX_EPS || !slot->ring_ok[ring_idx]) return -1;

    xhci_trb_t trb = {0}, ev;
    trb.param = buf_phys;
    trb.status = len & 0x1FFFFu;
    trb.control = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT) | TRB_IOC | (dir_in ? TRB_ISP : 0) |
                  (dir_in ? TRB_DIR_IN : 0);

    xhci_ring_push(&slot->rings[ring_idx], &trb);
    xhci_doorbell(x, (uint32_t) slot->slot_id, (uint32_t) dci);

    int r = xhci_wait_event(x, TRB_TYPE_TRANSFER_EVENT, &ev, 1000);
    if (r) return r;
    uint32_t cc = (ev.status >> 24) & 0xFF;
    uint32_t residue = ev.status & 0xFFFFFFu;
    if (actual) *actual = (int) (len - residue);
    if (cc == COMP_SUCCESS || cc == COMP_SHORT_PACKET) return 0;
    if (cc == COMP_STALL_ERROR) return USB_STALL;
    return USB_BABBLE;
}

static int xhci_control(usb_hc_t *hc, usb_device_t *dev, const usb_setup_pkt_t *setup, void *buf,
                        int len, int *actual) {
    xhci_t *x = (xhci_t *) hc->priv;

    if (dev->addr == 0) {
        xhci_slot_t *slot = NULL;
        for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
            if (x->slots[i].active && x->slots[i].addr == 0 && x->slots[i].root_port == dev->port)
                slot = &x->slots[i];
        }
        if (!slot) {
            int slot_id = xhci_enable_slot(x);
            if (slot_id <= 0 || slot_id > XHCI_MAX_SLOTS) return -1;
            slot = &x->slots[slot_id - 1];
            memset(slot, 0, sizeof(*slot));
            slot->slot_id = slot_id;
            slot->root_port = (uint8_t) dev->port;
            slot->speed = (uint8_t) dev->speed;
            slot->active = true;

            slot->in_ctx_phys = (uint64_t) pmm_alloc_zeroed();
            slot->dev_ctx_phys = (uint64_t) pmm_alloc_zeroed();
            if (!slot->in_ctx_phys || !slot->dev_ctx_phys) return -1;
            slot->in_ctx = (xhci_input_ctx_t *) phys_to_virt(slot->in_ctx_phys);
            slot->dev_ctx = (xhci_dev_ctx_t *) phys_to_virt(slot->dev_ctx_phys);
            x->dcbaa[slot_id] = slot->dev_ctx_phys;
            __asm__ volatile("" ::: "memory");

            uint64_t ring_phys = (uint64_t) pmm_alloc_zeroed();
            if (!ring_phys) return -1;
            xhci_ring_init(&slot->rings[0], (xhci_trb_t *) phys_to_virt(ring_phys), ring_phys);
            slot->rings[0].trbs[TR_RING_SIZE - 1].control |= slot->rings[0].cycle;
            slot->ring_ok[0] = true;

            memset(slot->in_ctx, 0, x->ctx_size * 32);
            slot->in_ctx->ctrl.add_flags = 0x3u;
            xhci_slot_ctx_t *sc = &slot->in_ctx->slot;
            sc->dw[0] = (1u << SLOT_CTX_ENTRIES_SHIFT) |
                        ((uint32_t) dev->speed << SLOT_CTX_SPEED_SHIFT) |
                        ((uint32_t) dev->port << SLOT_CTX_ROOT_PORT_SHIFT);
            sc->dw[1] = 0;
            sc->dw[2] = 0;
            sc->dw[3] = 0;

            int mps = 8;
            xhci_ep_ctx_t *ep0 = &slot->in_ctx->ep[0];
            ep0->dw[0] = 0;
            ep0->dw[1] = (3u << EP_CTX_CERR_SHIFT) | (EP_TYPE_CONTROL << EP_CTX_EP_TYPE_SHIFT) |
                         ((uint32_t) mps << EP_CTX_MPS_SHIFT);
            ep0->dw[2] = (uint32_t) (slot->rings[0].phys | 1u);
            ep0->dw[3] = (uint32_t) (slot->rings[0].phys >> 32);
            ep0->dw[4] = 8;

            int r = xhci_address_device(x, slot_id, slot, true);
            if (r) return r;
            g_xhci_pending_dev = dev;
        }
        dev->hc->priv = x;
        g_xhci_pending_dev = dev;

        xhci_ring_t *ring = &slot->rings[0];
        xhci_trb_t trb_setup = {0}, ev;
        trb_setup.param = 0;
        memcpy(&trb_setup.param, setup, 8);
        trb_setup.status = 8;
        trb_setup.control = (TRB_TYPE_SETUP << TRB_TYPE_SHIFT) | TRB_IDT |
                            ((uint32_t) (len > 0 ? ((setup->bmRequestType & USB_REQTYPE_DIR_IN)
                                                        ? TRB_TRT_IN
                                                        : TRB_TRT_OUT)
                                                 : TRB_TRT_NO_DATA)
                             << TRB_TRT_SHIFT);
        xhci_ring_push(ring, &trb_setup);

        if (len > 0) {
            xhci_trb_t trb_data = {0};
            trb_data.param = virt_to_phys(buf);
            trb_data.status = (uint32_t) len & 0x1FFFFu;
            trb_data.control = (TRB_TYPE_DATA << TRB_TYPE_SHIFT) |
                               ((setup->bmRequestType & USB_REQTYPE_DIR_IN) ? TRB_DIR_IN : 0);
            xhci_ring_push(ring, &trb_data);
        }

        xhci_trb_t trb_status = {0};
        trb_status.param = 0;
        trb_status.status = 0;
        trb_status.control = (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) | TRB_IOC |
                             ((len > 0 && (setup->bmRequestType & USB_REQTYPE_DIR_IN)) ? 0
                                                                                        : TRB_DIR_IN);
        xhci_ring_push(ring, &trb_status);
        xhci_doorbell(x, (uint32_t) slot->slot_id, 1);

        int r = xhci_wait_event(x, TRB_TYPE_TRANSFER_EVENT, &ev, 1000);
        if (r) return r;
        uint32_t cc = (ev.status >> 24) & 0xFF;
        uint32_t residue = ev.status & 0xFFFFFFu;
        if (len > 0 && actual) *actual = (int) (len - residue);
        if (cc == COMP_SUCCESS || cc == COMP_SHORT_PACKET) return 0;
        if (cc == COMP_STALL_ERROR) return USB_STALL;
        return USB_BABBLE;
    }

    xhci_slot_t *slot = NULL;
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (x->slots[i].active && x->slots[i].root_port == dev->port) slot = &x->slots[i];
    }
    if (!slot) return -1;

    if (!slot->addressed) {
        memset(slot->in_ctx, 0, x->ctx_size * 32);
        slot->in_ctx->ctrl.add_flags = 0x3u;
        xhci_slot_ctx_t *sc = &slot->in_ctx->slot;
        sc->dw[0] = (1u << SLOT_CTX_ENTRIES_SHIFT) |
                    ((uint32_t) dev->speed << SLOT_CTX_SPEED_SHIFT) |
                    ((uint32_t) dev->port << SLOT_CTX_ROOT_PORT_SHIFT);
        int mps = dev->max_packet0;
        if (mps <= 0) mps = (dev->speed >= USB_SPEED_SUPER) ? 512 : 64;
        xhci_ep_ctx_t *ep0 = &slot->in_ctx->ep[0];
        ep0->dw[1] = (3u << EP_CTX_CERR_SHIFT) | (EP_TYPE_CONTROL << EP_CTX_EP_TYPE_SHIFT) |
                     ((uint32_t) mps << EP_CTX_MPS_SHIFT);
        ep0->dw[2] = (uint32_t) (slot->rings[0].phys | 1u);
        ep0->dw[3] = (uint32_t) (slot->rings[0].phys >> 32);
        ep0->dw[4] = 8;
        int r = xhci_address_device(x, slot->slot_id, slot, false);
        if (r) return r;
        slot->addr = (uint8_t) dev->addr;
        slot->addressed = true;
        return 0;
    }

    xhci_ring_t *ring = &slot->rings[0];
    xhci_trb_t trb_setup = {0}, ev;
    memcpy(&trb_setup.param, setup, 8);
    trb_setup.status = 8;
    trb_setup.control = (TRB_TYPE_SETUP << TRB_TYPE_SHIFT) | TRB_IDT |
                        ((uint32_t) (len > 0 ? ((setup->bmRequestType & USB_REQTYPE_DIR_IN)
                                                    ? TRB_TRT_IN
                                                    : TRB_TRT_OUT)
                                             : TRB_TRT_NO_DATA)
                         << TRB_TRT_SHIFT);
    xhci_ring_push(ring, &trb_setup);
    if (len > 0) {
        xhci_trb_t trb_data = {0};
        trb_data.param = virt_to_phys(buf);
        trb_data.status = (uint32_t) len & 0x1FFFFu;
        trb_data.control = (TRB_TYPE_DATA << TRB_TYPE_SHIFT) |
                           ((setup->bmRequestType & USB_REQTYPE_DIR_IN) ? TRB_DIR_IN : 0);
        xhci_ring_push(ring, &trb_data);
    }
    xhci_trb_t trb_status = {0};
    trb_status.control = (TRB_TYPE_STATUS << TRB_TYPE_SHIFT) | TRB_IOC |
                         ((len > 0 && (setup->bmRequestType & USB_REQTYPE_DIR_IN)) ? 0
                                                                                    : TRB_DIR_IN);
    xhci_ring_push(ring, &trb_status);
    xhci_doorbell(x, (uint32_t) slot->slot_id, 1);

    int r = xhci_wait_event(x, TRB_TYPE_TRANSFER_EVENT, &ev, 1000);
    if (r) return r;
    uint32_t cc = (ev.status >> 24) & 0xFF;
    uint32_t residue = ev.status & 0xFFFFFFu;
    if (len > 0 && actual) *actual = (int) (len - residue);
    if (cc == COMP_SUCCESS || cc == COMP_SHORT_PACKET) return 0;
    if (cc == COMP_STALL_ERROR) return USB_STALL;
    return USB_BABBLE;
}

static uint8_t g_xhci_toggles[XHCI_MAX_SLOTS][XHCI_MAX_EPS];

static int xhci_generic_xfer(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                             void *buf, int len, int *actual, uint8_t attr) {
    xhci_t *x = (xhci_t *) hc->priv;
    xhci_slot_t *slot = NULL;
    for (int i = 0; i < XHCI_MAX_SLOTS; i++) {
        if (x->slots[i].active && x->slots[i].root_port == dev->port &&
            x->slots[i].addr == dev->addr)
            slot = &x->slots[i];
    }
    if (!slot) return -1;

    int dci = xhci_ep_dci(ep_addr);
    if (!slot->ring_ok[dci - 1]) {
        uint16_t mps = 512;
        uint8_t ival = 10;
        for (int i = 0; i < dev->num_ifaces; i++)
            for (int e = 0; e < dev->ifaces[i].num_eps; e++)
                if (dev->ifaces[i].eps[e].addr == ep_addr) {
                    mps = dev->ifaces[i].eps[e].max_packet;
                    ival = dev->ifaces[i].eps[e].interval;
                }
        int r = xhci_ensure_ep(x, slot, ep_addr, attr, mps, ival);
        if (r) return r;
    }
    (void) toggle;
    return xhci_submit_data(x, slot, ep_addr, (ep_addr & 0x80) != 0, virt_to_phys(buf),
                            (uint32_t) len, actual);
}

static int xhci_bulk(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                     void *buf, int len, int *actual) {
    return xhci_generic_xfer(hc, dev, ep_addr, toggle, buf, len, actual, 2);
}

static int xhci_interrupt(usb_hc_t *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                          void *buf, int len, int *actual) {
    return xhci_generic_xfer(hc, dev, ep_addr, toggle, buf, len, actual, 3);
}

static usb_hc_ops_t g_xhci_ops = {
    .reset_port = xhci_reset_port,
    .control = xhci_control,
    .bulk = xhci_bulk,
    .interrupt = xhci_interrupt,
};

static int xhci_hw_init(xhci_t *x) {
    x->op[XHCI_OP_USBCMD / 4] = 0;
    usb_msleep(5);
    x->op[XHCI_OP_USBCMD / 4] = XHCI_CMD_HCRST;
    usb_msleep(50);
    uint32_t to = 2000;
    while (x->op[XHCI_OP_USBCMD / 4] & XHCI_CMD_HCRST) {
        if (!to--) return -1;
        usb_msleep(1);
    }
    to = 2000;
    while (x->op[XHCI_OP_USBSTS / 4] & XHCI_STS_CNR) {
        if (!to--) return -1;
        usb_msleep(1);
    }

    uint32_t pagesize = x->op[XHCI_OP_PAGESIZE / 4];
    x->page_shift = 12;
    while ((pagesize & 1u) == 0 && x->page_shift < 24) {
        pagesize >>= 1;
        x->page_shift++;
    }

    uint64_t mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return -1;
    x->dcbaa_phys = mem;
    x->dcbaa = (uint64_t *) phys_to_virt(mem);
    x->op[XHCI_OP_DCBAAP / 4] = (uint32_t) (mem & 0xFFFFFFFFu);
    x->op[XHCI_OP_DCBAAP / 4 + 1] = (uint32_t) (mem >> 32);

    mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return -1;
    xhci_ring_init(&x->cmd_ring, (xhci_trb_t *) phys_to_virt(mem), mem);
    uint64_t crcr = mem | 1u;
    x->op[XHCI_OP_CRCR / 4] = (uint32_t) (crcr & 0xFFFFFFFFu);
    x->op[XHCI_OP_CRCR / 4 + 1] = (uint32_t) (crcr >> 32);

    mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return -1;
    x->evt_ring_phys = mem;
    x->evt_ring = (xhci_trb_t *) phys_to_virt(mem);
    x->evt_deq = 0;
    x->evt_cycle = 1;

    mem = (uint64_t) pmm_alloc_zeroed();
    if (!mem) return -1;
    x->erst_phys = mem;
    x->erst = (xhci_erst_entry_t *) phys_to_virt(mem);
    x->erst[0].ring_base = x->evt_ring_phys;
    x->erst[0].ring_size = EVT_RING_SIZE;

    x->rt[XHCI_RT_ERSTSZ / 4] = 1;
    x->rt[XHCI_RT_ERSTBA / 4] = (uint32_t) (x->erst_phys & 0xFFFFFFFFu);
    x->rt[XHCI_RT_ERSTBA / 4 + 1] = (uint32_t) (x->erst_phys >> 32);
    x->rt[XHCI_RT_ERDP / 4] = (uint32_t) (x->evt_ring_phys & 0xFFFFFFFFu);
    x->rt[XHCI_RT_ERDP / 4 + 1] = (uint32_t) (x->evt_ring_phys >> 32);
    x->rt[XHCI_RT_IMAN / 4] = 0;
    x->rt[XHCI_RT_IMOD / 4] = 0;

    x->op[XHCI_OP_DNCTRL / 4] = 0xFFFF;
    x->op[XHCI_OP_CONFIG / 4] = (uint32_t) x->max_slots;
    x->op[XHCI_OP_USBCMD / 4] = XHCI_CMD_RUN;

    to = 2000;
    while (x->op[XHCI_OP_USBSTS / 4] & XHCI_STS_HCH) {
        if (!to--) return -1;
        usb_msleep(1);
    }
    usb_msleep(20);
    return 0;
}

static int xhci_probe_one(uint8_t bus, uint8_t dev, uint8_t fn) {
    if (g_nxhci >= 1) return 0;
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

    xhci_t *x = &g_xhci;
    memset(x, 0, sizeof(*x));
    x->hc.priv = x;

    uint16_t cmd = pci_read16(bus, dev, fn, 0x04);
    pci_write32(bus, dev, fn, 0x04, cmd | 0x06u);

    uint32_t pages = (bar_size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages < 4) pages = 4;
    if (pages > XHCI_MMIO_MAX_PAGES) pages = XHCI_MMIO_MAX_PAGES;
    for (uint32_t i = 0; i < pages; i++) {
        vmm_map(&g_kernel_space, XHCI_MMIO_VBASE + (uint64_t) i * PAGE_SIZE,
                (bar0 & PAGE_MASK) + (uint64_t) i * PAGE_SIZE, VMM_KDATA | VMM_PCD);
    }
    x->cap = (volatile uint8_t *) (XHCI_MMIO_VBASE + (bar0 & (PAGE_SIZE - 1)));

    uint8_t caplength = x->cap[XHCI_CAP_CAPLENGTH];
    uint32_t hcs1 = *(volatile uint32_t *) (x->cap + XHCI_CAP_HCSPARAMS1);
    uint32_t hcc1 = *(volatile uint32_t *) (x->cap + XHCI_CAP_HCCPARAMS1);
    uint32_t dboff = *(volatile uint32_t *) (x->cap + XHCI_CAP_DBOFF);
    uint32_t rtsoff = *(volatile uint32_t *) (x->cap + XHCI_CAP_RTSOFF);

    x->max_slots = (int) (hcs1 & 0xFFu);
    if (x->max_slots > XHCI_MAX_SLOTS) x->max_slots = XHCI_MAX_SLOTS;
    x->num_ports = (int) ((hcs1 >> 24) & 0xFFu);
    if (x->num_ports > 24) x->num_ports = 24;
    x->ctx_size = (hcc1 & 0x4u) ? 64 : 32;

    x->op = (volatile uint32_t *) (x->cap + caplength);
    x->rt = (volatile uint32_t *) (x->cap + (rtsoff & ~0x1Fu));
    x->db = (volatile uint32_t *) (x->cap + (dboff & ~0x3u));

    if (xhci_hw_init(x)) {
        log_warn("xHCI: hardware init failed");
        return 0;
    }

    x->hc.name = "xhci";
    x->hc.ops = &g_xhci_ops;
    x->hc.num_ports = x->num_ports;
    usb_hc_register(&x->hc);
    g_nxhci++;
    log_info("xHCI: %d ports, %d slots, ctx_size=%d", x->num_ports, x->max_slots,
             (int) x->ctx_size);
    return 1;
}

void xhci_init(void) {
    g_nxhci = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30)
            xhci_probe_one(d->bus, d->dev, d->fn);
    }
}
