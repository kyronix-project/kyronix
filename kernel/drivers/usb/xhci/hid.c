#include "../usb_glue.h"
#include "../xhci.h"
#include "../usb_hid.h"
#include "../lib/string.h"

#define XHCI_HID_RING_TRBS  64
#define XHCI_HID_REPORT_LEN USB_HID_REPORT_LEN
#define XHCI_MAX_HID        8

#define CC_XFER_SUCCESS       1
#define CC_XFER_SHORT_PACKET  13
#define HID_RECOVERY_MAX_FAILS 8

typedef struct {
    xhci_controller_t *ctl;
    uint8_t   slot_id;
    uint8_t   port_id;
    uint8_t   speed;
    uint8_t   ep_dci;
    uint8_t   ep_interval;
    uint16_t  ep_mps;
    xhci_trb_t *ring;
    uintptr_t  ring_phys;
    uint16_t   enq;
    uint8_t    cyc;
    uint8_t   *buf;
    uintptr_t  buf_phys;
    usb_hid_kbd_state_t state;
    bool       active;
    bool       is_mouse;
    uint32_t   total_reports;
    uint32_t   total_errors;
    volatile bool needs_recovery;
    uint8_t    last_err_cc;
    uint8_t    recovery_fails;
} xhci_hid_kbd_t;

static xhci_hid_kbd_t g_hid_kbds[XHCI_MAX_HID];
static int            g_hid_count = 0;

static void hid_post_normal(xhci_hid_kbd_t *k) {
    xhci_trb_t *t = &k->ring[k->enq];
    t->parameter = (uint64_t)k->buf_phys;
    t->status    = XHCI_HID_REPORT_LEN;
    uint32_t ctl = TRB_TYPE(TRB_NORMAL) | TRB_IOC;
    if (k->cyc) ctl |= TRB_CYCLE;
    __asm__ volatile("" ::: "memory");
    t->control = ctl;

    k->enq++;
    if (k->enq == XHCI_HID_RING_TRBS - 1) {
        xhci_trb_t *link = &k->ring[XHCI_HID_RING_TRBS - 1];
        uint32_t lctl = TRB_TYPE(TRB_LINK) | TRB_TC;
        if (k->cyc) lctl |= TRB_CYCLE;
        link->control = lctl;
        k->enq = 0;
        k->cyc ^= 1;
    }

    __asm__ volatile("" ::: "memory");
    k->ctl->doorbells[k->slot_id] = k->ep_dci;
}

static void hid_reset_ring(xhci_hid_kbd_t *k) {
    memset(k->ring, 0, XHCI_HID_RING_TRBS * sizeof(xhci_trb_t));
    xhci_trb_t *link = &k->ring[XHCI_HID_RING_TRBS - 1];
    link->parameter = (uint64_t)k->ring_phys;
    link->control   = TRB_TYPE(TRB_LINK) | TRB_TC;
    k->enq = 0;
    k->cyc = 1;
}

static int configure_intr_ep(xhci_controller_t *c, uint8_t slot_id,
                             uint8_t port_id, uint8_t speed,
                             uint8_t ep_dci, uint16_t mps, uint8_t bInterval,
                             uintptr_t tr_phys)
{
    uintptr_t inctx_phys;
    uint8_t *inctx = (uint8_t *)dma_alloc_coherent(4096, &inctx_phys);
    if (!inctx) return -ENOMEM;
    memset(inctx, 0, 4096);

    uint32_t *icc = (uint32_t *)inctx;
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << ep_dci);

    (void)port_id;
    const uint32_t *out_slot =
        (const uint32_t *)phys_to_virt((uint64_t)c->dcbaa[slot_id]);

    uint32_t *slot_ctx = xhci_input_context(c, inctx, 0);
    slot_ctx[0] = (out_slot[0] & ~(0x1Fu << 27)) | ((uint32_t)ep_dci << 27);
    slot_ctx[1] = out_slot[1];
    slot_ctx[2] = out_slot[2];

    uint8_t interval = bInterval;
    if (speed == 1 || speed == 2) {
        uint32_t mframes = (uint32_t)bInterval * 8;
        uint8_t ivl = 0;
        while ((uint32_t)(1u << (ivl + 1)) <= mframes) ivl++;
        if (ivl < 3)  ivl = 3;
        if (ivl > 10) ivl = 10;
        interval = ivl;
    } else {
        interval = bInterval ? (uint8_t)(bInterval - 1) : 0;
        if (interval > 15) interval = 15;
    }

    uint32_t *ep_ctx = xhci_input_context(c, inctx, ep_dci);
    ep_ctx[0] = ((uint32_t)interval << 16);
    ep_ctx[1] = (3u << 1) | ((uint32_t)EP_TYPE_INTR_IN << 3) | ((uint32_t)mps << 16);
    ep_ctx[2] = (uint32_t)(tr_phys | 1);
    ep_ctx[3] = (uint32_t)((uint64_t)tr_phys >> 32);
    ep_ctx[4] = (uint32_t)mps | ((uint32_t)mps << 16);

    xhci_trb_t ev;
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0,
                          TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                          ((uint32_t)slot_id << 24),
                          &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] CONFIGURE_ENDPOINT: cc=%u\n", cc);
        return -EIO;
    }
    return 0;
}

static int hid_set_protocol(xhci_controller_t *c, uint8_t slot_id,
                            xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                            uint16_t *enq, uint8_t *cyc,
                            uint8_t intf, uint8_t protocol)
{

    int r = -EIO;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = xhci_control_xfer(c, slot_id, ep0_ring, ep0_phys, enq, cyc,
                              0x21, 0x0B, (uint16_t)protocol, intf, 0, NULL);
        if (r == 0) return 0;
        usb_msleep(25);
    }
    return r;
}

static int hid_set_idle(xhci_controller_t *c, uint8_t slot_id,
                        xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                        uint16_t *enq, uint8_t *cyc,
                        uint8_t intf, uint8_t duration)
{
    uint16_t wValue = (uint16_t)duration << 8;
    return xhci_control_xfer(c, slot_id, ep0_ring, ep0_phys, enq, cyc,
                             0x21, 0x0A, wValue, intf, 0, NULL);
}

static void hid_read_report_desc(xhci_controller_t *c, uint8_t slot_id,
                                 xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                                 uint16_t *enq, uint8_t *cyc, uint8_t intf)
{
    uintptr_t phys;
    uint8_t *rd = (uint8_t *)dma_alloc_coherent(256, &phys);
    if (!rd) return;
    memset(rd, 0, 256);

    int r = xhci_control_xfer(c, slot_id, ep0_ring, ep0_phys, enq, cyc,
                              0x81, 0x06, 0x2200, intf, 128, rd);
    serial_printf("[xhci]   HID report descriptor read: intf=%u r=%d\n", intf, r);
    dma_free_coherent(rd, 256);
}

static void hid_set_report_led(xhci_controller_t *c, uint8_t slot_id,
                               xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                               uint16_t *enq, uint8_t *cyc, uint8_t intf)
{
    uintptr_t phys;
    uint8_t *led = (uint8_t *)dma_alloc_coherent(8, &phys);
    if (!led) return;
    memset(led, 0, 8);

    int r = xhci_control_xfer(c, slot_id, ep0_ring, ep0_phys, enq, cyc,
                              0x21, 0x09, 0x0200, intf, 1, led);
    serial_printf("[xhci]   HID SET_REPORT(LED=0): intf=%u r=%d\n", intf, r);
    dma_free_coherent(led, 8);
}

int xhci_hid_kbd_register(xhci_controller_t *c, uint8_t slot_id,
                          uint8_t port_id, uint8_t speed,
                          xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                          uint16_t *enq, uint8_t *cyc,
                          const usb_kbd_match_t *m)
{
    if (g_hid_count >= XHCI_MAX_HID) return -ENOMEM;
    uint8_t ep_num = m->ep_addr & 0x0F;
    uint8_t ep_dci = (uint8_t)(2 * ep_num + 1);

    uintptr_t ring_phys;
    xhci_trb_t *ring = (xhci_trb_t *)dma_alloc_coherent(
        XHCI_HID_RING_TRBS * sizeof(xhci_trb_t), &ring_phys);
    if (!ring) return -ENOMEM;
    memset(ring, 0, XHCI_HID_RING_TRBS * sizeof(xhci_trb_t));
    xhci_trb_t *link = &ring[XHCI_HID_RING_TRBS - 1];
    link->parameter = (uint64_t)ring_phys;
    link->control   = TRB_TYPE(TRB_LINK) | TRB_TC;

    int r = configure_intr_ep(c, slot_id, port_id, speed,
                              ep_dci, m->ep_mps, m->ep_interval, ring_phys);
    if (r < 0) return r;

    hid_read_report_desc(c, slot_id, ep0_ring, ep0_phys, enq, cyc, m->intf);
    if (hid_set_protocol(c, slot_id, ep0_ring, ep0_phys, enq, cyc, m->intf, 0) < 0)
        serial_writestring("[xhci]   HID SET_PROTOCOL(boot) failed\n");
    hid_set_idle    (c, slot_id, ep0_ring, ep0_phys, enq, cyc, m->intf, 8);
    if (!m->is_mouse)
        hid_set_report_led(c, slot_id, ep0_ring, ep0_phys, enq, cyc, m->intf);

    uintptr_t buf_phys;
    uint8_t *buf = (uint8_t *)dma_alloc_coherent(64, &buf_phys);
    if (!buf) return -ENOMEM;
    memset(buf, 0, 64);

    xhci_hid_kbd_t *k = &g_hid_kbds[g_hid_count++];
    memset(k, 0, sizeof(*k));
    k->ctl          = c;
    k->slot_id      = slot_id;
    k->port_id      = port_id;
    k->speed        = speed;
    k->ep_dci       = ep_dci;
    k->ep_mps       = m->ep_mps;
    k->ep_interval  = m->ep_interval;
    k->ring         = ring;
    k->ring_phys    = ring_phys;
    k->enq          = 0;
    k->cyc          = 1;
    k->buf          = buf;
    k->buf_phys     = buf_phys;
    k->active       = true;
    k->is_mouse     = m->is_mouse;

    hid_post_normal(k);

    serial_printf("[xhci]   HID %s attached: slot=%u dci=%u mps=%u ivl=%u\n",
                  m->is_mouse ? "mouse" : "kbd",
                  slot_id, ep_dci, m->ep_mps, m->ep_interval);
    return 0;
}

int xhci_hid_kbd_active_count(void) {
    int n = 0;
    for (int i = 0; i < g_hid_count; i++)
        if (g_hid_kbds[i].active && !g_hid_kbds[i].is_mouse) n++;
    return n;
}

static void hid_recover_endpoint(xhci_hid_kbd_t *k) {
    xhci_controller_t *c = k->ctl;
    uint8_t cc_was = k->last_err_cc;
    k->needs_recovery = false;

    int r1 = xhci_reset_endpoint(c, k->slot_id, k->ep_dci);
    hid_reset_ring(k);
    int r2 = xhci_set_tr_dequeue(c, k->slot_id, k->ep_dci, k->ring_phys, 1);

    if (r1 < 0 || r2 < 0) {
        if (++k->recovery_fails >= HID_RECOVERY_MAX_FAILS) {
            k->active = false;
            serial_printf("[xhci] kbd slot=%u dci=%u: recovery failed %u times (last cc=%u) -> giving up\n",
                          k->slot_id, k->ep_dci, k->recovery_fails, cc_was);
        }
        return;
    }
    k->recovery_fails = 0;
    hid_post_normal(k);
    serial_printf("[xhci] kbd slot=%u dci=%u: endpoint recovered after cc=%u (total errors=%u), re-armed\n",
                  k->slot_id, k->ep_dci, cc_was, k->total_errors);
}

void xhci_hid_kbd_tick(void) {
    for (int i = 0; i < g_hid_count; i++) {
        xhci_hid_kbd_t *k = &g_hid_kbds[i];
        if (k->active && k->needs_recovery) hid_recover_endpoint(k);
    }

    usb_hid_kbd_state_t *states[XHCI_MAX_HID];
    int n = 0;
    for (int i = 0; i < g_hid_count; i++) {
        if (g_hid_kbds[i].active) states[n++] = &g_hid_kbds[i].state;
    }
    if (n > 0) usb_hid_kbd_tick_repeats(states, n);

    static uint32_t kick = 0;
    if (++kick >= 100) {
        kick = 0;
        for (int i = 0; i < g_hid_count; i++) {
            xhci_hid_kbd_t *k = &g_hid_kbds[i];
            if (!k->active || k->needs_recovery) continue;
            if (k->total_reports == 0) {
                __asm__ volatile("" ::: "memory");
                k->ctl->doorbells[k->slot_id] = k->ep_dci;
            }
        }
    }

}

void xhci_hid_kbd_disconnect_slot(uint8_t slot_id) {
    for (int i = 0; i < g_hid_count; i++) {
        if (g_hid_kbds[i].active && g_hid_kbds[i].slot_id == slot_id)
            g_hid_kbds[i].active = false;
    }
}

bool xhci_hid_kbd_handle_xfer_event(uint8_t slot_id, uint8_t dci, uint8_t cc) {
    for (int i = 0; i < g_hid_count; i++) {
        xhci_hid_kbd_t *k = &g_hid_kbds[i];
        if (k->active && k->slot_id == slot_id && k->ep_dci == dci) {
            if (cc != CC_XFER_SUCCESS && cc != CC_XFER_SHORT_PACKET) {
                k->total_errors++;
                k->last_err_cc    = cc;
                k->needs_recovery = true;
                return true;
            }
            k->total_reports++;
            if (k->is_mouse)
                usb_hid_mouse_process_report(k->buf, XHCI_HID_REPORT_LEN);
            else
                usb_hid_kbd_process_report(&k->state, k->buf);
            hid_post_normal(k);
            return true;
        }
    }
    return false;
}
