#include "../usb_glue.h"
#include "../usb_config.h"
#include "../usb_enum.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../xhci.h"

#define XHCI_CAP_CAPLENGTH    0x00
#define XHCI_CAP_HCIVERSION   0x02
#define XHCI_CAP_HCSPARAMS1   0x04
#define XHCI_CAP_HCSPARAMS2   0x08
#define XHCI_CAP_HCSPARAMS3   0x0C
#define XHCI_CAP_HCCPARAMS1   0x10
#define XHCI_CAP_DBOFF        0x14
#define XHCI_CAP_RTSOFF       0x18
#define XHCI_CAP_HCCPARAMS2   0x1C

#define XHCI_OP_USBCMD        0x00
#define XHCI_OP_USBSTS        0x04
#define XHCI_OP_PAGESIZE      0x08
#define XHCI_OP_DNCTRL        0x14
#define XHCI_OP_CRCR          0x18
#define XHCI_OP_DCBAAP        0x30
#define XHCI_OP_CONFIG        0x38
#define XHCI_OP_PORTSC(n)    (0x400 + (n) * 0x10)

#define XHCI_RT_IR0_IMAN      0x20
#define XHCI_RT_IR0_IMOD      0x24
#define XHCI_RT_IR0_ERSTSZ    0x28
#define XHCI_RT_IR0_ERSTBA    0x30
#define XHCI_RT_IR0_ERDP      0x38

#define USBCMD_RS             (1u << 0)
#define USBCMD_HCRST          (1u << 1)
#define USBCMD_INTE           (1u << 2)

#define USBSTS_HCH            (1u << 0)
#define USBSTS_CNR            (1u << 11)

#define IMAN_IP               (1u << 0)
#define IMAN_IE               (1u << 1)

#define ERDP_BUSY             (1u << 3)

#define TRB_CYCLE             (1u << 0)
#define TRB_TC                (1u << 1)
#define TRB_IOC               (1u << 5)

#define TRB_TYPE_SHIFT        10
#define TRB_TYPE(t)           ((uint32_t)(t) << TRB_TYPE_SHIFT)
#define TRB_TYPE_GET(c)       (((c) >> TRB_TYPE_SHIFT) & 0x3F)

#define TRB_NORMAL            1
#define TRB_SETUP             2
#define TRB_DATA              3
#define TRB_STATUS            4
#define TRB_LINK              6
#define TRB_NO_OP_CMD         23
#define TRB_ENABLE_SLOT       9
#define TRB_ADDRESS_DEVICE    11
#define TRB_RESET_DEVICE      17
#define TRB_RESET_ENDPOINT    14
#define TRB_SET_TR_DEQUEUE    16
#define TRB_ADDR_BSR          (1u << 9)
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT  13
#define TRB_TRANSFER_EVT      32
#define TRB_CMD_COMPLETION_EVT 33
#define TRB_PORT_STATUS_EVT   34

#define USBSTS_EINT           (1u << 3)

#define EP_TYPE_ISOCH_OUT     1
#define EP_TYPE_BULK_OUT      2
#define EP_TYPE_INTR_OUT      3
#define EP_TYPE_CONTROL       4
#define EP_TYPE_ISOCH_IN      5
#define EP_TYPE_BULK_IN       6
#define EP_TYPE_INTR_IN       7

#define TRB_IDT               (1u << 6)
#define TRB_DIR_IN            (1u << 16)

#define TRB_TRT_NO_DATA       0
#define TRB_TRT_OUT_DATA      2
#define TRB_TRT_IN_DATA       3

#define CC_SUCCESS            1
#define CC_SHORT_PACKET       13
#define CC_STALL_ERROR        6

#define USB_GET_DESCRIPTOR    6
#define USB_SET_CONFIGURATION 9
#define USB_DESC_DEVICE       1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_INTERFACE    4
#define USB_DESC_ENDPOINT     5

#define PORTSC_CCS            (1u << 0)
#define PORTSC_PED            (1u << 1)
#define PORTSC_PR             (1u << 4)
#define PORTSC_PP             (1u << 9)
#define PORTSC_SPEED_SHIFT    10
#define PORTSC_SPEED_MASK     0xF

static xhci_controller_t g_controllers[XHCI_MAX_CONTROLLERS];
static int g_count = 0;

static inline uint32_t cap_r32(xhci_controller_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->bar0 + off);
}
static inline uint8_t cap_r8(xhci_controller_t *c, uint32_t off) {
    uint32_t dw = cap_r32(c, off & ~3u);
    return (uint8_t)((dw >> ((off & 3u) * 8)) & 0xFFu);
}
static inline uint16_t cap_r16(xhci_controller_t *c, uint32_t off) {
    uint32_t dw = cap_r32(c, off & ~3u);
    return (uint16_t)((dw >> ((off & 3u) * 8)) & 0xFFFFu);
}

static inline uint32_t op_r32(xhci_controller_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->op_regs + off);
}
static inline void op_w32(xhci_controller_t *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->op_regs + off) = v;
}
static inline void op_w64(xhci_controller_t *c, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(c->op_regs + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(c->op_regs + off + 4) = (uint32_t)(v >> 32);
}
static inline uint32_t rt_r32(xhci_controller_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->runtime_regs + off);
}
static inline void rt_w32(xhci_controller_t *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->runtime_regs + off) = v;
}
static inline void rt_w64(xhci_controller_t *c, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(c->runtime_regs + off)     = (uint32_t)(v & 0xFFFFFFFFu);
    *(volatile uint32_t *)(c->runtime_regs + off + 4) = (uint32_t)(v >> 32);
}

static const char *speed_name(uint8_t s) {
    switch (s) {
        case 1: return "Full-speed (12 Mb/s)";
        case 2: return "Low-speed  (1.5 Mb/s)";
        case 3: return "High-speed (480 Mb/s)";
        case 4: return "SuperSpeed (5 Gb/s)";
        case 5: return "SuperSpeed+ (10 Gb/s)";
        default: return "unknown";
    }
}

static int xhci_halt(xhci_controller_t *c) {
    uint32_t cmd_initial = op_r32(c, XHCI_OP_USBCMD);
    uint32_t sts_initial = op_r32(c, XHCI_OP_USBSTS);
    if (sts_initial & USBSTS_HCH) return 0;

    op_w32(c, XHCI_OP_USBCMD, cmd_initial & ~USBCMD_RS);

    for (int i = 0; i < 50; i++) {
        if (op_r32(c, XHCI_OP_USBSTS) & USBSTS_HCH) return 0;
        usb_msleep(1);
    }
    return -EIO;
}

static int xhci_reset(xhci_controller_t *c) {
    uint32_t cmd = op_r32(c, XHCI_OP_USBCMD);
    op_w32(c, XHCI_OP_USBCMD, cmd | USBCMD_HCRST);

    for (int i = 0; i < 200; i++) {
        if ((op_r32(c, XHCI_OP_USBCMD) & USBCMD_HCRST) == 0 &&
            (op_r32(c, XHCI_OP_USBSTS) & USBSTS_CNR)   == 0)
            return 0;
        usb_msleep(1);
    }
    return -EIO;
}

static int xhci_alloc_rings(xhci_controller_t *c) {
    size_t dcbaa_bytes = ((size_t)(c->max_slots + 1)) * sizeof(uint64_t);
    if (dcbaa_bytes < 4096) dcbaa_bytes = 4096;
    c->dcbaa = (uint64_t *)dma_alloc_coherent(dcbaa_bytes, &c->dcbaa_phys);
    if (!c->dcbaa) return -ENOMEM;
    memset(c->dcbaa, 0, dcbaa_bytes);

    uint32_t sp_hi = (c->hcsparams2 >> 21) & 0x1F;
    uint32_t sp_lo = (c->hcsparams2 >> 27) & 0x1F;
    c->max_scratchpad = (sp_hi << 5) | sp_lo;

    if (c->max_scratchpad > 0) {
        size_t arr_bytes = (size_t)c->max_scratchpad * sizeof(uint64_t);
        if (arr_bytes < 4096) arr_bytes = 4096;
        c->scratchpad_array =
            (uint64_t *)dma_alloc_coherent(arr_bytes, &c->scratchpad_array_phys);
        if (!c->scratchpad_array) return -ENOMEM;
        memset(c->scratchpad_array, 0, arr_bytes);

        for (uint32_t i = 0; i < c->max_scratchpad; i++) {
            uintptr_t p;
            void *buf = dma_alloc_coherent(4096, &p);
            if (!buf) return -ENOMEM;
            c->scratchpad_array[i] = (uint64_t)p;
        }
        c->dcbaa[0] = (uint64_t)c->scratchpad_array_phys;
    }

    c->cmd_ring = (xhci_trb_t *)dma_alloc_coherent(
        XHCI_CMD_RING_TRBS * sizeof(xhci_trb_t), &c->cmd_ring_phys);
    if (!c->cmd_ring) return -ENOMEM;
    memset(c->cmd_ring, 0, XHCI_CMD_RING_TRBS * sizeof(xhci_trb_t));

    xhci_trb_t *link = &c->cmd_ring[XHCI_CMD_RING_TRBS - 1];
    link->parameter = (uint64_t)c->cmd_ring_phys;
    link->status    = 0;
    link->control   = TRB_TYPE(TRB_LINK) | TRB_TC;
    c->cmd_enqueue = 0;
    c->cmd_cycle   = 1;

    c->evt_ring = (xhci_trb_t *)dma_alloc_coherent(
        XHCI_EVT_RING_TRBS * sizeof(xhci_trb_t), &c->evt_ring_phys);
    if (!c->evt_ring) return -ENOMEM;
    memset(c->evt_ring, 0, XHCI_EVT_RING_TRBS * sizeof(xhci_trb_t));
    c->evt_dequeue = 0;
    c->evt_cycle   = 1;

    c->erst = (xhci_erst_entry_t *)dma_alloc_coherent(64, &c->erst_phys);
    if (!c->erst) return -ENOMEM;
    memset(c->erst, 0, 64);
    c->erst[0].ring_segment_base = (uint64_t)c->evt_ring_phys;
    c->erst[0].ring_segment_size = XHCI_EVT_RING_TRBS;
    return 0;
}

static void xhci_setup_rings(xhci_controller_t *c) {
    op_w64(c, XHCI_OP_DCBAAP, (uint64_t)c->dcbaa_phys);
    op_w64(c, XHCI_OP_CRCR,   (uint64_t)c->cmd_ring_phys | 1);

    rt_w32(c, XHCI_RT_IR0_ERSTSZ, 1);
    rt_w64(c, XHCI_RT_IR0_ERDP,   (uint64_t)c->evt_ring_phys);
    rt_w64(c, XHCI_RT_IR0_ERSTBA, (uint64_t)c->erst_phys);

    rt_w32(c, XHCI_RT_IR0_IMAN, IMAN_IP);
}

static int xhci_start(xhci_controller_t *c) {
    uint32_t cmd = op_r32(c, XHCI_OP_USBCMD);
    cmd |= USBCMD_RS | USBCMD_INTE;
    op_w32(c, XHCI_OP_USBCMD, cmd);

    for (int i = 0; i < 200; i++) {
        if ((op_r32(c, XHCI_OP_USBSTS) & USBSTS_HCH) == 0) {
            c->running = true;
            return 0;
        }
        usb_msleep(1);
    }
    return -EIO;
}

static void xhci_cmd_enqueue(xhci_controller_t *c, uint64_t param,
                             uint32_t status, uint32_t control,
                             uintptr_t *trb_phys_out)
{
    xhci_trb_t *slot = &c->cmd_ring[c->cmd_enqueue];
    slot->parameter = param;
    slot->status    = status;

    uint32_t ctl = control & ~TRB_CYCLE;
    if (c->cmd_cycle) ctl |= TRB_CYCLE;

    __asm__ volatile("" ::: "memory");
    slot->control = ctl;

    if (trb_phys_out)
        *trb_phys_out = c->cmd_ring_phys + (uintptr_t)c->cmd_enqueue * sizeof(xhci_trb_t);

    c->cmd_enqueue++;
    if (c->cmd_enqueue == XHCI_CMD_RING_TRBS - 1) {
        xhci_trb_t *link = &c->cmd_ring[XHCI_CMD_RING_TRBS - 1];
        uint32_t lctl = TRB_TYPE(TRB_LINK) | TRB_TC;
        if (c->cmd_cycle) lctl |= TRB_CYCLE;
        link->control = lctl;
        c->cmd_enqueue = 0;
        c->cmd_cycle ^= 1;
    }

    __asm__ volatile("" ::: "memory");
    c->doorbells[0] = 0;
}

static void xhci_handle_xfer_event(xhci_controller_t *c, const xhci_trb_t *ev);

static int xhci_poll_event(xhci_controller_t *c, uintptr_t wait_trb_phys,
                           xhci_trb_t *out_event, uint64_t timeout_ms)
{
    uint64_t start = usb_now_ns();
    uint64_t deadline_ns = timeout_ms * 1000000ULL;

    for (;;) {

        while (__atomic_exchange_n(&c->drain_lock, 1, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");

        xhci_trb_t snap;
        bool have = false;

        xhci_trb_t *e = &c->evt_ring[c->evt_dequeue];
        uint32_t ctl = e->control;
        if ((ctl & TRB_CYCLE) == c->evt_cycle) {
            snap = *e;

            uint16_t new_dq = (uint16_t)(c->evt_dequeue + 1);
            if (new_dq >= XHCI_EVT_RING_TRBS) {
                new_dq = 0;
                c->evt_cycle ^= 1;
            }
            c->evt_dequeue = new_dq;

            uint64_t erdp =
                (uint64_t)c->evt_ring_phys + (uint64_t)c->evt_dequeue * sizeof(xhci_trb_t);
            rt_w64(c, XHCI_RT_IR0_ERDP, erdp | ERDP_BUSY);
            have = true;
        }
        __atomic_store_n(&c->drain_lock, 0, __ATOMIC_RELEASE);

        if (have) {
            uint32_t type = TRB_TYPE_GET(snap.control);
            if (type == TRB_CMD_COMPLETION_EVT || type == TRB_TRANSFER_EVT) {
                if (wait_trb_phys == 0 || snap.parameter == (uint64_t)wait_trb_phys) {
                    if (out_event) *out_event = snap;
                    return 0;
                }
            }

            if (type == TRB_TRANSFER_EVT)
                xhci_handle_xfer_event(c, &snap);
            continue;
        }

        if (usb_now_ns() - start > deadline_ns) return -EIO;
        __asm__ volatile("pause");
    }
}

int xhci_send_cmd(xhci_controller_t *c, uint64_t param,
                         uint32_t status, uint32_t control,
                         xhci_trb_t *out_event)
{
    uintptr_t cmd_phys;
    xhci_cmd_enqueue(c, param, status, control, &cmd_phys);
    return xhci_poll_event(c, cmd_phys, out_event, 1000);
}

static int xhci_port_reset(xhci_controller_t *c, uint8_t port, uint8_t *out_speed) {
    uint32_t portsc = op_r32(c, XHCI_OP_PORTSC(port));
    if (!(portsc & PORTSC_CCS)) return -ENODEV;

    uint32_t preserve = portsc & ~((0xFu << 5) | (0x7Fu << 17));
    op_w32(c, XHCI_OP_PORTSC(port), preserve | PORTSC_PR);

    for (int i = 0; i < 1000; i++) {
        portsc = op_r32(c, XHCI_OP_PORTSC(port));
        if ((portsc & PORTSC_PR) == 0) break;
        usb_msleep(1);
    }
    if (portsc & PORTSC_PR) return -EIO;

    for (int i = 0; i < 1000; i++) {
        portsc = op_r32(c, XHCI_OP_PORTSC(port));
        if (portsc & PORTSC_PED) break;
        usb_msleep(1);
    }
    if (!(portsc & PORTSC_PED)) return -EIO;

    *out_speed = (uint8_t)((portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK);

    op_w32(c, XHCI_OP_PORTSC(port), portsc & ~((0xFu << 5) | (1u << 1)));
    return 0;
}

static uint16_t default_max_packet0(uint8_t speed) {
    switch (speed) {
        case 1: return 8;
        case 2: return 8;
        case 3: return 64;
        case 4: return 512;
        case 5: return 512;
        default: return 8;
    }
}

static int xhci_address_device(xhci_controller_t *c, const xhci_topology_t *topo,
                               uint8_t slot_id, uintptr_t ep0_ring_phys,
                               int bsr, uintptr_t *out_inctx_phys)
{
    uintptr_t devctx_phys;
    void *devctx = dma_alloc_coherent(4096, &devctx_phys);
    if (!devctx) return -ENOMEM;
    memset(devctx, 0, 4096);
    c->dcbaa[slot_id] = (uint64_t)devctx_phys;

    uintptr_t inctx_phys;
    uint8_t *inctx = (uint8_t *)dma_alloc_coherent(4096, &inctx_phys);
    if (!inctx) return -ENOMEM;
    memset(inctx, 0, 4096);

    uint32_t *icc = (uint32_t *)inctx;
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << 1);

    uint32_t *slot_ctx = xhci_input_context(c, inctx, 0);
    slot_ctx[0] = (topo->route_string & 0xFFFFFu)
                | ((uint32_t)topo->speed << 20)
                | (1u << 27);
    slot_ctx[1] = (uint32_t)topo->root_hub_port << 16;
    slot_ctx[2] = (uint32_t)topo->parent_slot
                | ((uint32_t)topo->parent_port << 8);

    uint32_t *ep0_ctx = xhci_input_context(c, inctx, 1);
    uint16_t mps0 = default_max_packet0(topo->speed);
    ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps0 << 16);
    ep0_ctx[2] = (uint32_t)(ep0_ring_phys | 1);
    ep0_ctx[3] = (uint32_t)((uint64_t)ep0_ring_phys >> 32);
    ep0_ctx[4] = 8;

    uint32_t control = TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);
    if (bsr) control |= TRB_ADDR_BSR;

    xhci_trb_t ev;
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0, control, &ev);
    if (r < 0) {
        serial_writestring("[xhci] ADDRESS DEVICE: timeout\n");
        return r;
    }
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] ADDRESS DEVICE (bsr=%d): completion code %u\n", bsr, cc);
        return -EIO;
    }
    if (out_inctx_phys) *out_inctx_phys = inctx_phys;
    return 0;
}

static int xhci_reset_device(xhci_controller_t *c, uint8_t slot_id) {
    xhci_trb_t ev;
    int r = xhci_send_cmd(c, 0, 0,
                          TRB_TYPE(TRB_RESET_DEVICE) | ((uint32_t)slot_id << 24), &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] RESET_DEVICE: cc=%u\n", cc);
        return -EIO;
    }
    return 0;
}

int xhci_reset_endpoint(xhci_controller_t *c, uint8_t slot_id, uint8_t dci) {
    xhci_trb_t ev;
    uint32_t control = TRB_TYPE(TRB_RESET_ENDPOINT) |
                       ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16);
    int r = xhci_send_cmd(c, 0, 0, control, &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS && cc != 19) {
        serial_printf("[xhci] RESET_ENDPOINT slot=%u dci=%u: cc=%u\n", slot_id, dci, cc);
        return -EIO;
    }
    return 0;
}

int xhci_set_tr_dequeue(xhci_controller_t *c, uint8_t slot_id, uint8_t dci,
                        uintptr_t deq_phys, uint8_t dcs) {
    xhci_trb_t ev;
    uint64_t param = (deq_phys & ~0xFULL) | (uint64_t)(dcs & 1u);
    uint32_t control = TRB_TYPE(TRB_SET_TR_DEQUEUE) |
                       ((uint32_t)slot_id << 24) | ((uint32_t)dci << 16);
    int r = xhci_send_cmd(c, param, 0, control, &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] SET_TR_DEQUEUE slot=%u dci=%u: cc=%u\n", slot_id, dci, cc);
        return -EIO;
    }
    return 0;
}

static int xhci_readdress_device(xhci_controller_t *c, uint8_t slot_id,
                                 uintptr_t inctx_phys) {
    xhci_trb_t ev;
    uint32_t control = TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot_id << 24);
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0, control, &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] re-ADDRESS: cc=%u\n", cc);
        return -EIO;
    }
    return 0;
}

int xhci_control_xfer(xhci_controller_t *c, uint8_t slot_id,
                             xhci_trb_t *ep0_ring, uintptr_t ep0_ring_phys,
                             uint16_t *enq, uint8_t *cyc,
                             uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                             void *data_out)
{
    if (wLength > 4096) return -EINVAL;

    uintptr_t buf_phys = 0;
    uint8_t *buf = NULL;
    if (wLength > 0) {
        buf = (uint8_t *)dma_alloc_coherent(4096, &buf_phys);
        if (!buf) return -ENOMEM;
        memset(buf, 0, 4096);
    }

    bool dir_in = (bmRequestType & 0x80) != 0;

    uint8_t setup[8] = {
        bmRequestType, bRequest,
        (uint8_t)(wValue & 0xFF), (uint8_t)((wValue >> 8) & 0xFF),
        (uint8_t)(wIndex & 0xFF), (uint8_t)((wIndex >> 8) & 0xFF),
        (uint8_t)(wLength & 0xFF), (uint8_t)((wLength >> 8) & 0xFF)
    };
    uint64_t setup_imm;
    memcpy(&setup_imm, setup, 8);

    uint32_t trt;
    if      (wLength == 0) trt = TRB_TRT_NO_DATA;
    else if (dir_in)       trt = TRB_TRT_IN_DATA;
    else                   trt = TRB_TRT_OUT_DATA;

    xhci_trb_t *t;

    t = &ep0_ring[*enq];
    t->parameter = setup_imm;
    t->status    = 8;
    {
        uint32_t ctl = TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16);
        if (*cyc) ctl |= TRB_CYCLE;
        t->control = ctl;
    }
    (*enq)++;

    if (wLength > 0) {
        t = &ep0_ring[*enq];
        t->parameter = (uint64_t)buf_phys;
        t->status    = wLength;
        {
            uint32_t ctl = TRB_TYPE(TRB_DATA);
            if (dir_in) ctl |= TRB_DIR_IN;
            if (*cyc) ctl |= TRB_CYCLE;
            t->control = ctl;
        }
        (*enq)++;
    }

    t = &ep0_ring[*enq];
    t->parameter = 0;
    t->status    = 0;
    {
        uint32_t ctl = TRB_TYPE(TRB_STATUS) | TRB_IOC;
        bool status_in = (wLength == 0) || !dir_in;
        if (status_in) ctl |= TRB_DIR_IN;
        if (*cyc) ctl |= TRB_CYCLE;
        t->control = ctl;
    }
    uintptr_t status_phys = ep0_ring_phys + (uintptr_t)(*enq) * sizeof(xhci_trb_t);
    (*enq)++;

    __asm__ volatile("" ::: "memory");
    c->doorbells[slot_id] = 1;

    xhci_trb_t ev;
    int r = xhci_poll_event(c, status_phys, &ev, 2000);
    if (r < 0) {
        if (buf) dma_free_coherent(buf, 4096);
        return r;
    }
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        if (buf) dma_free_coherent(buf, 4096);
        serial_printf("[xhci] control xfer (req=0x%02x type=0x%02x): cc=%u\n",
                      bRequest, bmRequestType, cc);
        if (cc == CC_STALL_ERROR) {
            uintptr_t deq = ep0_ring_phys + (uintptr_t)(*enq) * sizeof(xhci_trb_t);
            if (xhci_reset_endpoint(c, slot_id, 1) == 0)
                xhci_set_tr_dequeue(c, slot_id, 1, deq, *cyc);
        }
        return -EIO;
    }

    if (data_out && wLength > 0) memcpy(data_out, buf, wLength);
    if (buf) dma_free_coherent(buf, 4096);
    return 0;
}

static const char *usb_class_name(uint8_t c) {
    switch (c) {
        case 0x00: return "(per-interface)";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC Data";
        case 0x0B: return "Smart Card";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFF: return "Vendor";
        default:   return "?";
    }
}

static const char *xfer_type_name(uint8_t a) {
    switch (a & 0x3) {
        case 0: return "Control";
        case 1: return "Isoch";
        case 2: return "Bulk";
        case 3: return "Interrupt";
        default: return "?";
    }
}

static void xhci_parse_config(const uint8_t *buf, uint16_t total) {
    if (total < 9) return;

    uint8_t n_intf    = buf[4];
    uint8_t cfg_val   = buf[5];
    uint8_t attrs     = buf[7];
    uint16_t max_ma   = (uint16_t)buf[8] * 2;

    serial_printf("[xhci]   config #%u: %u interface(s)  attrs=0x%02x  power=%u mA\n",
                  cfg_val, n_intf, attrs, max_ma);

    uint16_t pos = 9;
    while (pos + 2 <= total) {
        uint8_t blen  = buf[pos];
        uint8_t btype = buf[pos + 1];
        if (blen < 2 || pos + blen > total) break;

        if (btype == USB_DESC_INTERFACE && blen >= 9) {
            uint8_t inum  = buf[pos + 2];
            uint8_t alt   = buf[pos + 3];
            uint8_t neps  = buf[pos + 4];
            uint8_t cls   = buf[pos + 5];
            uint8_t sub   = buf[pos + 6];
            uint8_t proto = buf[pos + 7];
            serial_printf("[xhci]     interface %u.%u: class=0x%02x (%s) "
                          "sub=0x%02x proto=0x%02x  endpoints=%u\n",
                          inum, alt, cls, usb_class_name(cls),
                          sub, proto, neps);
        } else if (btype == USB_DESC_ENDPOINT && blen >= 7) {
            uint8_t addr = buf[pos + 2];
            uint8_t attr = buf[pos + 3];
            uint16_t mps = (uint16_t)buf[pos + 4] | ((uint16_t)buf[pos + 5] << 8);
            uint8_t ivl  = buf[pos + 6];
            uint8_t num  = addr & 0x0F;
            const char *dir = (addr & 0x80) ? "IN" : "OUT";
            serial_printf("[xhci]       ep %u %s  type=%s  mps=%u  interval=%u\n",
                          num, dir, xfer_type_name(attr), mps & 0x7FF, ivl);
        }
        pos += blen;
    }
}

#define XHCI_MAX_USB_DEVS 32
static xhci_pub_dev_t g_usb_devs[XHCI_MAX_USB_DEVS];
static int g_usb_count = 0;

int xhci_list_devs(xhci_pub_dev_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = (g_usb_count < max) ? g_usb_count : max;
    for (int i = 0; i < n; i++) out[i] = g_usb_devs[i];
    return n;
}

static void xhci_handle_xfer_event(xhci_controller_t *c, const xhci_trb_t *ev) {
    (void)c;
    uint8_t  slot_id = (uint8_t)((ev->control >> 24) & 0xFFu);
    uint8_t  dci     = (uint8_t)((ev->control >> 16) & 0x1Fu);
    uint8_t  cc      = (uint8_t)((ev->status  >> 24) & 0xFFu);
    uint32_t resid   = ev->status & 0x00FFFFFFu;

    if (xhci_hid_kbd_handle_xfer_event(slot_id, dci, cc)) return;
    if (xhci_msc_handle_xfer_event(slot_id, dci, cc, resid, ev->parameter)) return;
}

void xhci_drain_events(xhci_controller_t *c) {
    uint64_t rflags;
    __asm__ volatile("pushfq; cli; pop %0" : "=r"(rflags) :: "memory");

    while (__atomic_exchange_n(&c->drain_lock, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");

    for (;;) {
        xhci_trb_t *e = &c->evt_ring[c->evt_dequeue];
        uint32_t ctl = e->control;
        if ((ctl & TRB_CYCLE) != c->evt_cycle) break;

        xhci_trb_t snap = *e;

        uint16_t new_dq = (uint16_t)(c->evt_dequeue + 1);
        if (new_dq >= XHCI_EVT_RING_TRBS) {
            new_dq = 0;
            c->evt_cycle ^= 1;
        }
        c->evt_dequeue = new_dq;

        uint32_t type = TRB_TYPE_GET(snap.control);
        if (type == TRB_TRANSFER_EVT)
            xhci_handle_xfer_event(c, &snap);
    }

    uint64_t erdp =
        (uint64_t)c->evt_ring_phys + (uint64_t)c->evt_dequeue * sizeof(xhci_trb_t);
    rt_w64(c, XHCI_RT_IR0_ERDP, erdp | ERDP_BUSY);

    __atomic_store_n(&c->drain_lock, 0, __ATOMIC_RELEASE);

    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

typedef struct {
    xhci_controller_t *ctl;
    uint8_t     slot_id;
    xhci_trb_t *ep0_ring;
    uintptr_t   ep0_phys;
    uint16_t   *enq;
    uint8_t    *cyc;
} xhci_ctrl_handle_t;

static int xhci_enum_control(void *h, const uint8_t setup[8],
                             void *data, uint16_t len, bool in) {
    (void)in; (void)len;
    xhci_ctrl_handle_t *xh = (xhci_ctrl_handle_t *)h;
    uint8_t  bmReqType = setup[0];
    uint8_t  bRequest  = setup[1];
    uint16_t wValue    = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8);
    uint16_t wIndex    = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8);
    uint16_t wLength   = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8);
    return xhci_control_xfer(xh->ctl, xh->slot_id, xh->ep0_ring, xh->ep0_phys,
                             xh->enq, xh->cyc, bmReqType, bRequest,
                             wValue, wIndex, wLength, data);
}

static int xhci_evaluate_ep0_mps(xhci_controller_t *c, uint8_t slot_id, uint16_t mps)
{
    uintptr_t inctx_phys;
    uint8_t *inctx = (uint8_t *)dma_alloc_coherent(4096, &inctx_phys);
    if (!inctx) return -ENOMEM;
    memset(inctx, 0, 4096);

    uint32_t *icc = (uint32_t *)inctx;
    icc[1] = (1u << 1);

    uint32_t *ep0_ctx = xhci_input_context(c, inctx, 1);
    ep0_ctx[1] = (3u << 1) | (4u << 3) | ((uint32_t)mps << 16);

    xhci_trb_t ev;
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0,
                          TRB_TYPE(TRB_EVALUATE_CONTEXT) | ((uint32_t)slot_id << 24),
                          &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] EVALUATE CONTEXT (mps=%u): completion code %u\n", mps, cc);
        return -EIO;
    }
    return 0;
}

void enumerate_device(xhci_controller_t *c, const xhci_topology_t *topo,
                      const char *path_label)
{
    xhci_trb_t ev;
    int r = xhci_send_cmd(c, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ev);
    if (r < 0 || ((ev.status >> 24) & 0xFFu) != CC_SUCCESS) {
        serial_printf("[xhci] %s: ENABLE SLOT failed\n", path_label);
        return;
    }
    uint8_t slot_id = (uint8_t)((ev.control >> 24) & 0xFFu);

    uintptr_t ep0_phys;
    xhci_trb_t *ep0_ring =
        (xhci_trb_t *)dma_alloc_coherent(XHCI_CMD_RING_TRBS * sizeof(xhci_trb_t),
                                         &ep0_phys);
    if (!ep0_ring) return;
    memset(ep0_ring, 0, XHCI_CMD_RING_TRBS * sizeof(xhci_trb_t));
    xhci_trb_t *link = &ep0_ring[XHCI_CMD_RING_TRBS - 1];
    link->parameter = (uint64_t)ep0_phys;
    link->control   = TRB_TYPE(TRB_LINK) | TRB_TC;

    uintptr_t inctx_phys = 0;
    if (xhci_address_device(c, topo, slot_id, ep0_phys, 1, &inctx_phys) < 0) return;
    xhci_reset_device(c, slot_id);
    if (xhci_readdress_device(c, slot_id, inctx_phys) < 0) return;

    uint16_t enq = 0;
    uint8_t  cyc = 1;
    xhci_ctrl_handle_t xh = { c, slot_id, ep0_ring, ep0_phys, &enq, &cyc };
    usb_ctrl_ops_t ops = { &xh, xhci_enum_control };

    if (topo->speed == 1 || topo->speed == 2) {
        uint8_t d8[8] = {0};
        uint8_t setup8[8] = { 0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 8, 0x00 };
        if (xhci_enum_control(&xh, setup8, d8, 8, true) >= 0 &&
            topo->speed == 1 &&
            d8[7] && d8[7] != default_max_packet0(1)) {
            serial_printf("[xhci] %s: FS EP0 mps %u -> evaluate context\n",
                          path_label, d8[7]);
            xhci_evaluate_ep0_mps(c, slot_id, d8[7]);
        }
    }

    uint8_t  desc[18];
    if (usb_get_device_descriptor(&ops, desc) < 0)
        return;

    uint16_t bcd_usb = (uint16_t)desc[2] | ((uint16_t)desc[3] << 8);
    uint16_t vid     = (uint16_t)desc[8] | ((uint16_t)desc[9] << 8);
    uint16_t pid     = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    uint16_t bcd_dev = (uint16_t)desc[12] | ((uint16_t)desc[13] << 8);
    uint8_t  dcls    = desc[4];
    uint8_t  n_cfg   = desc[17];

    serial_printf("[xhci] %s slot %u: USB %u.%u  VID=0x%04x PID=0x%04x  "
                  "rev=%u.%u  class=0x%02x (%s)  EP0 MPS=%u  nConfigs=%u\n",
                  path_label, slot_id,
                  (bcd_usb >> 8), (bcd_usb >> 4) & 0xF,
                  vid, pid,
                  (bcd_dev >> 8), (bcd_dev >> 4) & 0xF,
                  dcls, usb_class_name(dcls),
                  (unsigned)desc[7], (unsigned)n_cfg);

    if (n_cfg == 0) return;

    uint8_t cfg_full[512];
    uint16_t total = 0;
    uint8_t  cfg_value = 0;
    if (usb_get_config(&ops, cfg_full, sizeof(cfg_full), &total, &cfg_value) < 0) {
        serial_writestring("[xhci]   GET_DESCRIPTOR(config): failed\n");
        return;
    }

    xhci_parse_config(cfg_full, total);

    if (usb_set_configuration(&ops, cfg_value) < 0) {
        serial_writestring("[xhci]   SET_CONFIGURATION: failed\n");
        return;
    }
    serial_printf("[xhci]   SET_CONFIGURATION %u: ok\n", cfg_value);

    usb_ifaces_t ifaces;
    usb_parse_config(cfg_full, total, &ifaces);
    uint8_t intf_cls   = ifaces.first_class;
    uint8_t intf_sub   = ifaces.first_sub;
    uint8_t intf_proto = ifaces.first_proto;

    int dev_idx = -1;
    for (int i = 0; i < g_usb_count; i++) {
        if (!g_usb_devs[i].present) { dev_idx = i; break; }
    }
    if (dev_idx < 0 && g_usb_count < XHCI_MAX_USB_DEVS) dev_idx = g_usb_count++;
    if (dev_idx >= 0) {
        xhci_pub_dev_t *u = &g_usb_devs[dev_idx];
        memset(u, 0, sizeof(*u));
        u->ctrl_idx     = (uint8_t)(c - g_controllers);
        u->slot_id      = slot_id;
        u->port_id      = topo->root_hub_port;
        u->speed        = topo->speed;
        u->vid          = vid;
        u->pid          = pid;
        u->bcd_dev      = bcd_dev;
        u->bcd_usb      = bcd_usb;
        u->dev_class    = dcls;
        u->dev_sub      = desc[5];
        u->dev_proto    = desc[6];
        u->intf_class   = intf_cls;
        u->intf_sub     = intf_sub;
        u->intf_proto   = intf_proto;
        u->ep0_mps      = desc[7];
        u->n_configs    = n_cfg;
        u->parent_slot  = topo->parent_slot;
        u->parent_port  = topo->parent_port;
        u->depth        = topo->depth;
        u->route_string = topo->route_string;
        u->present      = 1;
    }

    bool is_hub = (dcls == 0x09) || (intf_cls == 0x09);

    if (is_hub) {
        if (xhci_hub_register(c, slot_id, topo,
                              ep0_ring, ep0_phys, &enq, &cyc) < 0)
            serial_writestring("[xhci]   Hub register failed\n");
        return;
    }

    if (ifaces.hid.present && ifaces.hid.in_ep) {
        usb_kbd_match_t kbd;
        memset(&kbd, 0, sizeof(kbd));
        kbd.found       = true;
        kbd.is_mouse    = ifaces.hid.is_mouse;
        kbd.intf        = ifaces.hid.intf;
        kbd.ep_addr     = ifaces.hid.in_ep | 0x80;
        kbd.ep_mps      = ifaces.hid.in_mps;
        kbd.ep_interval = ifaces.hid.interval;
        if (xhci_hid_kbd_register(c, slot_id, topo->root_hub_port, topo->speed,
                                  ep0_ring, ep0_phys, &enq, &cyc, &kbd) < 0)
            serial_writestring("[xhci]   HID register failed\n");
    }

    if (ifaces.msc.present && ifaces.msc.in_ep && ifaces.msc.out_ep) {
        usb_msc_match_t msc;
        memset(&msc, 0, sizeof(msc));
        msc.found    = true;
        msc.intf     = ifaces.msc.intf;
        msc.in_addr  = ifaces.msc.in_ep | 0x80;
        msc.out_addr = ifaces.msc.out_ep;
        msc.in_mps   = ifaces.msc.in_mps;
        msc.out_mps  = ifaces.msc.out_mps;
        if (xhci_msc_register(c, slot_id, topo->root_hub_port, topo->speed,
                              &msc) < 0)
            serial_writestring("[xhci]   MSC register failed\n");
    }
}

static void xhci_enumerate_port(xhci_controller_t *c, int port) {

    usb_msleep(100);
    if (!(op_r32(c, XHCI_OP_PORTSC(port)) & PORTSC_CCS)) return;

    uint8_t speed;
    int r = -EIO;
    for (int attempt = 0; attempt < 2; attempt++) {
        r = xhci_port_reset(c, (uint8_t)port, &speed);
        if (r == 0) break;
        usb_msleep(50);
    }
    if (r < 0) {
        serial_printf("[xhci] port %d reset failed (%d)\n", port + 1, r);
        return;
    }

    usb_msleep(10);

    xhci_topology_t topo = {
        .speed         = speed,
        .root_hub_port = (uint8_t)(port + 1),
        .route_string  = 0,
        .parent_slot   = 0,
        .parent_port   = 0,
        .depth         = 0,
    };
    char label[16];
    snprintf(label, sizeof(label), "port %d", port + 1);
    enumerate_device(c, &topo, label);
}

static void xhci_enumerate_ports(xhci_controller_t *c) {
    int found = 0;
    for (int p = 0; p < c->max_ports; p++) {
        __asm__ volatile("mfence" ::: "memory");
        uint32_t portsc = op_r32(c, XHCI_OP_PORTSC(p));
        __asm__ volatile("mfence" ::: "memory");
        if (!(portsc & PORTSC_CCS)) continue;
        xhci_enumerate_port(c, p);
        found++;
    }
    if (found == 0) serial_writestring("[xhci] no devices connected\n");
}

static void xhci_dump_ports(xhci_controller_t *c) {
    for (uint8_t p = 0; p < c->max_ports; p++) {
        uint32_t portsc = op_r32(c, XHCI_OP_PORTSC(p));
        bool ccs = (portsc & PORTSC_CCS) != 0;
        bool ped = (portsc & PORTSC_PED) != 0;
        bool pp  = (portsc & PORTSC_PP)  != 0;
        uint8_t speed = (portsc >> PORTSC_SPEED_SHIFT) & PORTSC_SPEED_MASK;
        if (ccs || ped) {
            serial_printf("[xhci]   port %u: ccs=%d ped=%d pp=%d speed=%u (%s)\n",
                          (unsigned)p, ccs, ped, pp,
                          (unsigned)speed, speed_name(speed));
        }
    }
}

static void xhci_bios_handoff(xhci_controller_t *c) {
    uint32_t xecp = (c->hccparams1 >> 16) & 0xFFFF;
    if (xecp == 0) return;
    uint32_t off = xecp << 2;

    for (int guard = 0; guard < 64; guard++) {
        volatile uint32_t *cap = (volatile uint32_t *)(c->bar0 + off);
        uint32_t val   = cap[0];
        uint8_t  id    = val & 0xFF;
        uint8_t  next  = (val >> 8) & 0xFF;

        if (id == 1) {
            if (val & (1u << 16)) {
                cap[0] = val | (1u << 24);
                int i;
                for (i = 0; i < 1000; i++) {
                    val = cap[0];
                    if (!(val & (1u << 16))) break;
                    for (volatile int d = 0; d < 50000; d++) ;
                }
                if (val & (1u << 16)) {
                    serial_writestring("[xhci] BIOS won't release ownership; forcing OS-only\n");
                    cap[0] = (cap[0] & ~(1u << 16)) | (1u << 24);
                }
            } else {
                cap[0] = val | (1u << 24);
            }

            uint32_t ctl = cap[1];
            ctl &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15));
            ctl |=  ((1u << 29) | (1u << 30) | (1u << 31));
            cap[1] = ctl;

            serial_writestring("[xhci] BIOS handoff complete\n");
            return;
        }

        if (next == 0) break;
        off += (uint32_t)next << 2;
    }
}

static void xhci_probe(pci_dev_t *pd) {
    if (g_count >= XHCI_MAX_CONTROLLERS) return;

    uint64_t bar_base = pd->bars[0];
    if (bar_base == 0) {
        serial_writestring("[xhci] BAR0 not memory or zero\n");
        return;
    }

    uint64_t bar_size = pd->bar_sizes[0] ? pd->bar_sizes[0] : 0x10000;
    bar_size = (bar_size + 0xFFFULL) & ~0xFFFULL;

    volatile uint8_t *bar_virt =
        (volatile uint8_t *)mmio_map((uintptr_t)bar_base, (size_t)bar_size);

    uint32_t cmd_reg = pci_read32(pd->bus, pd->dev, pd->fn, 0x04);
    cmd_reg |= 0x2   | 0x4   | (0x400  );
    pci_write32(pd->bus, pd->dev, pd->fn, 0x04, cmd_reg);

    xhci_controller_t *c = &g_controllers[g_count];
    memset(c, 0, sizeof(*c));
    c->bar0     = bar_virt;
    c->bar_size = (uint32_t)bar_size;

    c->caplength   = cap_r8(c,  XHCI_CAP_CAPLENGTH);
    c->hciversion  = cap_r16(c, XHCI_CAP_HCIVERSION);
    c->hcsparams1  = cap_r32(c, XHCI_CAP_HCSPARAMS1);
    c->hcsparams2  = cap_r32(c, XHCI_CAP_HCSPARAMS2);
    c->hcsparams3  = cap_r32(c, XHCI_CAP_HCSPARAMS3);
    c->hccparams1  = cap_r32(c, XHCI_CAP_HCCPARAMS1);
    c->dboff       = cap_r32(c, XHCI_CAP_DBOFF)  & ~0x3u;
    c->rtsoff      = cap_r32(c, XHCI_CAP_RTSOFF) & ~0x1Fu;
    c->hccparams2  = cap_r32(c, XHCI_CAP_HCCPARAMS2);

    c->max_slots = (uint8_t) (c->hcsparams1 & 0xFF);
    c->max_intrs = (uint16_t)((c->hcsparams1 >> 8)  & 0x7FF);
    c->max_ports = (uint8_t) ((c->hcsparams1 >> 24) & 0xFF);

    c->op_regs      = bar_virt + c->caplength;
    c->runtime_regs = bar_virt + c->rtsoff;
    c->doorbells    = (volatile uint32_t *)(bar_virt + c->dboff);

    serial_printf("[xhci] BAR0=0x%llx size=%u  HCIVERSION=%u.%u\n",
                  (unsigned long long)bar_base, c->bar_size,
                  (c->hciversion >> 8) & 0xFF, c->hciversion & 0xFF);
    serial_printf("[xhci] caplength=%u dboff=0x%x rtsoff=0x%x\n",
                  c->caplength, c->dboff, c->rtsoff);
    serial_printf("[xhci] HCSPARAMS1=0x%x  slots=%u intrs=%u ports=%u\n",
                  c->hcsparams1, c->max_slots, c->max_intrs, c->max_ports);
    serial_printf("[xhci] HCCPARAMS1=0x%x  AC64=%d CSZ=%d ext_caps_off=0x%x\n",
                  c->hccparams1,
                  c->hccparams1 & 1, (c->hccparams1 >> 2) & 1,
                  ((c->hccparams1 >> 16) & 0xFFFF) << 2);

    xhci_bios_handoff(c);

    printf("[xhci] probe: slots=%u ports=%u, halting...\n", c->max_slots, c->max_ports);
    (void)xhci_halt(c);
    if (xhci_reset(c) < 0) {
        serial_writestring("[xhci] reset timeout\n");
        printf("[xhci] RESET TIMEOUT\n");
        return;
    }
    printf("[xhci] reset ok, alloc rings...\n");

    uint32_t cfg = op_r32(c, XHCI_OP_CONFIG);
    cfg = (cfg & ~0xFFu) | c->max_slots;
    op_w32(c, XHCI_OP_CONFIG, cfg);
    serial_printf("[xhci] CONFIG.MaxSlotsEn = %u\n", c->max_slots);

    if (xhci_alloc_rings(c) < 0) {
        serial_writestring("[xhci] failed to alloc rings\n");
        return;
    }
    serial_printf("[xhci] rings: cmd@0x%llx evt@0x%llx erst@0x%llx scratchpad=%u pages\n",
                  (unsigned long long)c->cmd_ring_phys,
                  (unsigned long long)c->evt_ring_phys,
                  (unsigned long long)c->erst_phys,
                  c->max_scratchpad);

    xhci_setup_rings(c);

    printf("[xhci] rings ok, starting...\n");
    if (xhci_start(c) < 0) {
        serial_writestring("[xhci] start (USBCMD.RS) timeout\n");
        printf("[xhci] START TIMEOUT\n");
        return;
    }
    serial_writestring("[xhci] controller running\n");

    for (int p = 0; p < c->max_ports; p++) {
        uint32_t portsc = op_r32(c, XHCI_OP_PORTSC(p));
        if (!(portsc & PORTSC_PP)) {
            op_w32(c, XHCI_OP_PORTSC(p), portsc | PORTSC_PP);
            serial_printf("[xhci] port %d: power on requested (PP was 0)\n", p + 1);
        }
    }
    usb_msleep(20);

    serial_writestring("[xhci] port snapshot (only those with device/connected):\n");
    xhci_dump_ports(c);

    printf("[xhci] enumerating ports...\n");
    xhci_enumerate_ports(c);
    printf("[xhci] ports enumerated\n");

    xhci_hub_post_init();
    xhci_msc_post_init();

    c->ready = true;
    g_count++;
}

void xhci_start_worker(void) {
    static bool spawned = false;
    if (spawned) return;
    if (g_count == 0) return;
    spawned = true;
    extern void xhci_worker(void);
    proc_create_kernel("xhci_worker", xhci_worker);
}

static void disconnect_slot(uint8_t slot_id) {
    for (int i = 0; i < g_usb_count; i++) {
        if (g_usb_devs[i].present && g_usb_devs[i].slot_id == slot_id) {
            g_usb_devs[i].present = 0;
            serial_printf("[xhci-hp] slot %u disconnected (vid:pid=%04x:%04x)\n",
                          slot_id, g_usb_devs[i].vid, g_usb_devs[i].pid);
        }
    }
    xhci_hid_kbd_disconnect_slot(slot_id);
    xhci_msc_disconnect_slot(slot_id);
    xhci_hub_disconnect_slot(slot_id);
}

void xhci_hcd_disconnect_subtree(uint8_t root_slot) {
    disconnect_slot(root_slot);
    for (int i = 0; i < g_usb_count; i++) {
        xhci_pub_dev_t *u = &g_usb_devs[i];
        if (u->present && u->parent_slot == root_slot) {
            xhci_hcd_disconnect_subtree(u->slot_id);
        }
    }
}

int xhci_hcd_find_root_dev(xhci_controller_t *c, uint8_t root_port) {
    uint8_t ctrl_idx = (uint8_t)(c - g_controllers);
    for (int i = 0; i < g_usb_count; i++) {
        xhci_pub_dev_t *u = &g_usb_devs[i];
        if (u->present && u->ctrl_idx == ctrl_idx &&
            u->port_id == root_port && u->depth == 0)
            return i;
    }
    return -1;
}

int xhci_hcd_find_hub_child(xhci_controller_t *c, uint32_t hub_route,
                            uint8_t hub_depth, uint8_t hub_port, uint8_t root_hub_port)
{
    (void)c;
    for (int i = 0; i < g_usb_count; i++) {
        xhci_pub_dev_t *u = &g_usb_devs[i];
        if (!u->present) continue;
        if (u->depth != hub_depth + 1) continue;
        if (u->port_id != root_hub_port) continue;
        uint32_t expected = hub_route | ((uint32_t)(hub_port & 0xF) << (4 * hub_depth));
        if (u->route_string == expected) return i;
    }
    return -1;
}

uint8_t xhci_hcd_slot_at_index(int idx) {
    if (idx < 0 || idx >= g_usb_count) return 0;
    return g_usb_devs[idx].slot_id;
}

static void xhci_handle_root_port(xhci_controller_t *c, int port_idx) {
    uint32_t portsc = op_r32(c, XHCI_OP_PORTSC(port_idx));
    uint32_t change = portsc & (0x7Fu << 17);
    if (change == 0) return;

    uint32_t preserve = portsc & ~((0xFu << 5) | (0x7Fu << 17));
    op_w32(c, XHCI_OP_PORTSC(port_idx), preserve | change);

    bool connected = (portsc & PORTSC_CCS) != 0;
    int existing = xhci_hcd_find_root_dev(c, (uint8_t)(port_idx + 1));

    if (connected && existing < 0) {
        serial_printf("[xhci-hp] root port %d: connect\n", port_idx + 1);
        xhci_enumerate_port(c, port_idx);
    } else if (!connected && existing >= 0) {
        serial_printf("[xhci-hp] root port %d: disconnect\n", port_idx + 1);
        xhci_hcd_disconnect_subtree(g_usb_devs[existing].slot_id);
    }
}

void xhci_worker(void) {
    serial_writestring("[xhci-worker] started\n");
    uint32_t slow_tick = 0;
    while (1) {
        int n_ctl = xhci_controller_count();
        for (int i = 0; i < n_ctl; i++) {
            xhci_controller_t *c = xhci_get_controller(i);
            if (!c || !c->ready) continue;
            xhci_drain_events(c);
        }

        xhci_hid_kbd_tick();

        if ((slow_tick++ % 25) == 0) {
            for (int i = 0; i < n_ctl; i++) {
                xhci_controller_t *c = xhci_get_controller(i);
                if (!c || !c->ready) continue;
                for (int p = 0; p < c->max_ports; p++)
                    xhci_handle_root_port(c, p);
            }
            xhci_hub_tick();
        }

        usb_msleep(4);
    }
}

void xhci_init(void) {
    usb_pci_for_each(0x0C, 0x03, 0x30, xhci_probe);
}

int xhci_controller_count(void) { return g_count; }
xhci_controller_t *xhci_get_controller(int idx) {
    if (idx < 0 || idx >= g_count) return NULL;
    return &g_controllers[idx];
}
