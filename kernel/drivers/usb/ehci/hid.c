#include "../usb_glue.h"
#include "../usb_hid.h"
#include "../lib/string.h"
#include "../ehci.h"

#define EHCI_MAX_HID 4

typedef struct ehci_hid_kbd {
    ehci_controller_t *ctl;
    uint8_t   addr;
    uint8_t   intf;
    uint8_t   in_ep;
    uint16_t  in_mps;

    ehci_qh_t  *qh;
    uintptr_t   qh_phys;
    ehci_qtd_t *qtd;
    uintptr_t   qtd_phys;
    uint8_t    *report_buf;
    uintptr_t   report_phys;

    uint32_t   dt_bit;
    usb_hid_kbd_state_t state;
    bool       active;
    bool       is_mouse;
} ehci_hid_kbd_t;

static ehci_hid_kbd_t g_hid_kbds[EHCI_MAX_HID];
static int g_hid_count = 0;

static void hid_kbd_arm(ehci_hid_kbd_t *k) {
    k->qtd->next     = QTD_T;
    k->qtd->alt_next = QTD_T;
    k->qtd->token    = QTD_STATUS_ACTIVE | QTD_PID_IN | QTD_CERR_3 | QTD_IOC
                     | ((uint32_t)8 << QTD_TBT_SHIFT);
    qtd_set_buf(k->qtd, k->report_phys, 8);

    uint32_t old_cmd;
    if (ehci_async_pause(k->ctl, &old_cmd) < 0) return;
    k->qh->cur_qtd          = 0;
    k->qh->overlay_next     = (uint32_t)k->qtd_phys;
    k->qh->overlay_alt_next = QTD_T;
    k->qh->overlay_token    = k->dt_bit;
    __asm__ volatile("" ::: "memory");
    ehci_async_resume(k->ctl, old_cmd);
}

int ehci_hid_kbd_setup(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                       const ehci_hid_info_t *info)
{
    if (speed != EHCI_SPEED_HS) {
        serial_writestring("[ehci-hid] only HS devices supported\n");
        return -EIO;
    }

    uint8_t setup_proto[8] = { 0x21, 0x0B, 0x00, 0x00, info->intf, 0x00, 0x00, 0x00 };
    int r = -EIO;
    for (int attempt = 0; attempt < 3; attempt++) {
        r = ehci_control_xfer(c, addr, speed, 64, setup_proto, NULL, 0, false);
        if (r == 0) break;
        usb_msleep(25);
    }
    if (r < 0) serial_printf("[ehci-hid] SET_PROTOCOL failed: %d\n", r);

    uint8_t setup_idle[8] = { 0x21, 0x0A, 0x00, 0x08, info->intf, 0x00, 0x00, 0x00 };
    r = ehci_control_xfer(c, addr, speed, 64, setup_idle, NULL, 0, false);
    if (r < 0) serial_printf("[ehci-hid] SET_IDLE failed: %d\n", r);

    ehci_hid_kbd_t *k = NULL;
    bool reuse = false;
    for (int i = 0; i < g_hid_count; i++) {
        if (!g_hid_kbds[i].active && g_hid_kbds[i].qh) { k = &g_hid_kbds[i]; reuse = true; break; }
    }
    if (!k) {
        if (g_hid_count >= EHCI_MAX_HID) return -ENOMEM;
        k = &g_hid_kbds[g_hid_count++];
        memset(k, 0, sizeof(*k));
    }

    if (!reuse) {
        k->report_buf = (uint8_t *)dma_alloc_coherent_low(64, &k->report_phys);
        if (!k->report_buf || k->report_phys >= 0xFFFFFFFFULL) return -ENOMEM;
        k->qtd = alloc_qtd(&k->qtd_phys);
        if (!k->qtd) return -ENOMEM;
        k->qh = (ehci_qh_t *)dma_alloc_coherent_low(4096, &k->qh_phys);
        if (!k->qh || k->qh_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    }

    k->ctl      = c;
    k->addr     = addr;
    k->intf     = info->intf;
    k->in_ep    = info->in_ep;
    k->in_mps   = info->in_mps;
    k->dt_bit   = 0;
    k->is_mouse = info->is_mouse;
    usb_hid_kbd_state_init(&k->state);
    memset(k->report_buf, 0, 64);

    uint32_t saved_hlp = reuse ? k->qh->hlp : 0;
    memset(k->qh, 0, sizeof(*k->qh));
    if (reuse) k->qh->hlp = saved_hlp;
    k->qh->ep_chars = ((uint32_t)addr & 0x7F)
                    | ((uint32_t)info->in_ep << 8)
                    | ((uint32_t)speed << 12)
                    | ((uint32_t)info->in_mps << 16)
                    | (4u << 28);
    k->qh->ep_caps  = (1u << 30);
    k->qh->cur_qtd  = 0;
    k->qh->overlay_next     = QTD_T;
    k->qh->overlay_alt_next = QTD_T;
    k->qh->overlay_token    = QTD_STATUS_HALTED;

    if (!reuse) {
        uint32_t old_cmd;
        if (ehci_async_pause(c, &old_cmd) < 0) {
            dma_free_coherent(k->qh, 4096);
            return -EIO;
        }
        k->qh->hlp = c->async_head->hlp;
        __asm__ volatile("" ::: "memory");
        c->async_head->hlp = ((uint32_t)k->qh_phys) | QH_TYPE_QH;
        ehci_async_resume(c, old_cmd);
    }

    k->active = true;

    hid_kbd_arm(k);

    serial_printf("[ehci-hid] %s registered (addr=%u intf=%u ep=%u mps=%u)\n",
                  info->is_mouse ? "mouse" : "kbd",
                  addr, info->intf, info->in_ep, info->in_mps);
    return 0;
}

static void hid_kbd_poll(ehci_hid_kbd_t *k) {
    if (!k->active) return;
    uint32_t tok = k->qtd->token;
    if (tok & QTD_STATUS_ACTIVE) return;

    if (!(tok & (QTD_STATUS_HALTED | QTD_STATUS_DBE | QTD_STATUS_BABBLE |
                 QTD_STATUS_XACTERR | QTD_STATUS_MISSED))) {
        uint32_t residue = (tok >> QTD_TBT_SHIFT) & 0x7FFF;
        uint32_t got = (residue < 8) ? (8 - residue) : 0;
        if (k->is_mouse) {
            if (got >= 3) usb_hid_mouse_process_report(k->report_buf, (int)got);
        } else {
            if (got >= USB_HID_REPORT_LEN)
                usb_hid_kbd_process_report(&k->state, k->report_buf);
        }
    }
    k->dt_bit = k->qh->overlay_token & QTD_DT;
    hid_kbd_arm(k);
}

int ehci_hid_kbd_active_count(void) {
    int n = 0;
    for (int i = 0; i < g_hid_count; i++)
        if (g_hid_kbds[i].active && !g_hid_kbds[i].is_mouse) n++;
    return n;
}

void ehci_hid_kbd_tick(void) {
    for (int i = 0; i < g_hid_count; i++)
        hid_kbd_poll(&g_hid_kbds[i]);

    usb_hid_kbd_state_t *states[EHCI_MAX_HID];
    int n = 0;
    for (int i = 0; i < g_hid_count; i++)
        states[n++] = &g_hid_kbds[i].state;
    if (n > 0) usb_hid_kbd_tick_repeats(states, n);
}

void ehci_hid_kbd_deactivate_addr(uint8_t addr) {
    for (int j = 0; j < g_hid_count; j++) {
        if (g_hid_kbds[j].active && g_hid_kbds[j].addr == addr)
            g_hid_kbds[j].active = false;
    }
}
