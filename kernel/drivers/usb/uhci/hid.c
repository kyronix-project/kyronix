#include "../usb_glue.h"
#include "../usb_hid.h"
#include "../lib/string.h"
#include "../uhci.h"

#define UHCI_MAX_HID 4

typedef struct uhci_hid_kbd {
    uhci_controller_t *ctl;
    uint8_t  addr;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  low_speed;
    uint32_t dt;

    uhci_qh_t  *qh;
    uintptr_t   qh_phys;
    uhci_td_t  *td;
    uintptr_t   td_phys;
    uint8_t    *report_buf;
    uintptr_t   report_phys;

    usb_hid_kbd_state_t state;
    bool       active;
    bool       is_mouse;
} uhci_hid_kbd_t;

static uhci_hid_kbd_t g_hid_kbds[UHCI_MAX_HID];
static int            g_hid_count = 0;

static void hid_kbd_arm(uhci_hid_kbd_t *k) {
    k->td->link   = LP_T;
    k->td->status = TD_STATUS_ACTIVE | TD_CERR(3) | TD_IOC | TD_SPD
                  | (k->low_speed ? TD_LS_DEV : 0);
    k->td->token  = TOKEN(k->in_mps, k->dt, k->in_ep, k->addr, UHCI_PID_IN);
    k->td->buffer = (uint32_t)k->report_phys;
    __asm__ volatile("" ::: "memory");
    k->qh->qelp = (uint32_t)k->td_phys;
    __asm__ volatile("" ::: "memory");
}

int uhci_hid_kbd_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                       const uhci_hid_info_t *info)
{
    if (g_hid_count >= UHCI_MAX_HID) return -ENOMEM;

    uint8_t setup_proto[8] = { 0x21, 0x0B, 0x00, 0x00, info->intf, 0x00, 0x00, 0x00 };
    for (int attempt = 0; attempt < 3; attempt++) {
        if (uhci_control_xfer(c, addr, low_speed, low_speed ? 8 : 64,
                              setup_proto, NULL, 0, false) == 0)
            break;
        usb_msleep(25);
    }
    uint8_t setup_idle[8]  = { 0x21, 0x0A, 0x00, 0x08, info->intf, 0x00, 0x00, 0x00 };
    (void)uhci_control_xfer(c, addr, low_speed, low_speed ? 8 : 64,
                            setup_idle, NULL, 0, false);

    uhci_hid_kbd_t *k = &g_hid_kbds[g_hid_count];
    memset(k, 0, sizeof(*k));
    k->ctl       = c;
    k->addr      = addr;
    k->intf      = info->intf;
    k->in_ep     = info->in_ep;
    k->in_mps    = info->in_mps ? info->in_mps : 8;
    k->low_speed = low_speed ? 1 : 0;
    k->dt        = 0;
    k->is_mouse  = info->is_mouse;
    usb_hid_kbd_state_init(&k->state);

    k->report_buf = (uint8_t *)dma_alloc_coherent_low(64, &k->report_phys);
    if (!k->report_buf || k->report_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(k->report_buf, 0, 64);

    k->td = uhci_alloc_td(&k->td_phys);
    if (!k->td) return -ENOMEM;

    k->qh = (uhci_qh_t *)dma_alloc_coherent_low(4096, &k->qh_phys);
    if (!k->qh || k->qh_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(k->qh, 0, sizeof(*k->qh));
    k->qh->qhlp = LP_T;
    k->qh->qelp = LP_T;

    __asm__ volatile("" ::: "memory");
    k->qh->qhlp = c->ctrl_qh->qhlp;
    __asm__ volatile("" ::: "memory");
    c->ctrl_qh->qhlp = ((uint32_t)k->qh_phys) | LP_Q;
    __asm__ volatile("" ::: "memory");

    g_hid_count++;
    k->active = true;

    hid_kbd_arm(k);

    serial_printf("[uhci-hid] %s registered addr=%u ep=%u (%s)\n",
                  info->is_mouse ? "mouse" : "kbd",
                  addr, info->in_ep, low_speed ? "LS" : "FS");
    return 0;
}

static void hid_kbd_poll(uhci_hid_kbd_t *k) {
    if (!k->active) return;
    uint32_t st = k->td->status;
    if (st & TD_STATUS_ACTIVE) return;

    if (!(st & (TD_STATUS_STALLED | TD_STATUS_BABBLE | TD_STATUS_DBE |
                TD_STATUS_CRC | TD_STATUS_BITSTUF))) {
        uint32_t got = TD_ACT_LEN(st) + 1;
        if (got > 8) got = 8;
        if (k->is_mouse) {
            if (got >= 3) usb_hid_mouse_process_report(k->report_buf, (int)got);
        } else {
            if (got >= USB_HID_REPORT_LEN)
                usb_hid_kbd_process_report(&k->state, k->report_buf);
        }
        k->dt ^= 1;
    }
    hid_kbd_arm(k);
}

int uhci_hid_kbd_active_count(void) {
    int n = 0;
    for (int i = 0; i < g_hid_count; i++)
        if (g_hid_kbds[i].active && !g_hid_kbds[i].is_mouse) n++;
    return n;
}

void uhci_hid_kbd_tick(void) {
    for (int i = 0; i < g_hid_count; i++)
        hid_kbd_poll(&g_hid_kbds[i]);

    usb_hid_kbd_state_t *states[UHCI_MAX_HID];
    int n = 0;
    for (int i = 0; i < g_hid_count; i++)
        states[n++] = &g_hid_kbds[i].state;
    if (n > 0) usb_hid_kbd_tick_repeats(states, n);
}

void uhci_hid_kbd_deactivate_addr(uint8_t addr) {
    for (int j = 0; j < g_hid_count; j++) {
        if (g_hid_kbds[j].active && g_hid_kbds[j].addr == addr)
            g_hid_kbds[j].active = false;
    }
}
