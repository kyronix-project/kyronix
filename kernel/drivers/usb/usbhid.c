#include "usb.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../input.h"
#include "../tty.h"

#define HID_DT_HID 0x21
#define HID_DT_REPORT 0x22

#define USBHID_MAX_DEVS 8
#define KBD_REPORT_LEN 8
#define MOUSE_REPORT_LEN 8

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_addr;
    uint16_t mps;
    uint8_t interval;
    bool is_kbd;
    uint8_t *buf;
    uint64_t buf_phys;
    uint8_t prev[KBD_REPORT_LEN];
} usbhid_dev_t;

static usbhid_dev_t g_hid[USBHID_MAX_DEVS];
static int g_nhid;

static const uint16_t hid_usage_linuxkey[128] = {
    [0x04] = 30, [0x05] = 48, [0x06] = 46, [0x07] = 32, [0x08] = 18, [0x09] = 33,
    [0x0A] = 34, [0x0B] = 35, [0x0C] = 23, [0x0D] = 36, [0x0E] = 37, [0x0F] = 38,
    [0x10] = 50, [0x11] = 49, [0x12] = 24, [0x13] = 25, [0x14] = 16, [0x15] = 19,
    [0x16] = 31, [0x17] = 20, [0x18] = 22, [0x19] = 47, [0x1A] = 17, [0x1B] = 45,
    [0x1C] = 21, [0x1D] = 44, [0x1E] = 2,  [0x1F] = 3,  [0x20] = 4,  [0x21] = 5,
    [0x22] = 6,  [0x23] = 7,  [0x24] = 8,  [0x25] = 9,  [0x26] = 10, [0x27] = 11,
    [0x28] = 28, [0x29] = 1,  [0x2A] = 14, [0x2B] = 15, [0x2C] = 57, [0x2D] = 12,
    [0x2E] = 13, [0x2F] = 26, [0x30] = 27, [0x31] = 43, [0x32] = 43, [0x33] = 39,
    [0x34] = 40, [0x35] = 41, [0x36] = 51, [0x37] = 52, [0x38] = 53, [0x39] = 58,
    [0x3A] = 59, [0x3B] = 60, [0x3C] = 61, [0x3D] = 62, [0x3E] = 63, [0x3F] = 64,
    [0x40] = 65, [0x41] = 66, [0x42] = 67, [0x43] = 68, [0x44] = 87, [0x45] = 88,
    [0x46] = 99, [0x47] = 70, [0x48] = 119, [0x49] = 110, [0x4A] = 102, [0x4B] = 104,
    [0x4C] = 111, [0x4D] = 107, [0x4E] = 109, [0x4F] = 106, [0x50] = 105, [0x51] = 108,
    [0x52] = 103, [0x53] = 69, [0x54] = 98, [0x55] = 55, [0x56] = 74, [0x57] = 78,
    [0x58] = 96, [0x59] = 79, [0x5A] = 80, [0x5B] = 81, [0x5C] = 75, [0x5D] = 76,
    [0x5E] = 77, [0x5F] = 71, [0x60] = 72, [0x61] = 73, [0x62] = 82, [0x63] = 83,
    [0x64] = 86, [0x65] = 127, [0x66] = 116, [0x67] = 183, [0x68] = 184, [0x69] = 185,
    [0x6A] = 186, [0x6B] = 187, [0x6C] = 188, [0x6D] = 189, [0x6E] = 190, [0x6F] = 191,
    [0x70] = 192, [0x71] = 193, [0x72] = 194, [0x73] = 125, [0x74] = 126, [0x75] = 100,
};

static const char hid_ascii[128] = {
    [0x1E] = '1', [0x1F] = '2', [0x20] = '3', [0x21] = '4', [0x22] = '5',
    [0x23] = '6', [0x24] = '7', [0x25] = '8', [0x26] = '9', [0x27] = '0',
    [0x2D] = '-', [0x2E] = '=', [0x2A] = 8,   [0x2B] = 9,   [0x28] = 13,
    [0x04] = 'a', [0x05] = 'b', [0x06] = 'c', [0x07] = 'd', [0x08] = 'e',
    [0x09] = 'f', [0x0A] = 'g', [0x0B] = 'h', [0x0C] = 'i', [0x0D] = 'j',
    [0x0E] = 'k', [0x0F] = 'l', [0x10] = 'm', [0x11] = 'n', [0x12] = 'o',
    [0x13] = 'p', [0x14] = 'q', [0x15] = 'r', [0x16] = 's', [0x17] = 't',
    [0x18] = 'u', [0x19] = 'v', [0x1A] = 'w', [0x1B] = 'x', [0x1C] = 'y',
    [0x1D] = 'z', [0x2F] = '[', [0x30] = ']', [0x31] = '\\', [0x33] = ';',
    [0x34] = '\'', [0x35] = '`', [0x36] = ',', [0x37] = '.', [0x38] = '/',
    [0x2C] = ' ',
};

static const char hid_ascii_shift[128] = {
    [0x1E] = '!', [0x1F] = '@', [0x20] = '#', [0x21] = '$', [0x22] = '%',
    [0x23] = '^', [0x24] = '&', [0x25] = '*', [0x26] = '(', [0x27] = ')',
    [0x2D] = '_', [0x2E] = '+', [0x2A] = 8,   [0x2B] = 9,   [0x28] = 13,
    [0x04] = 'A', [0x05] = 'B', [0x06] = 'C', [0x07] = 'D', [0x08] = 'E',
    [0x09] = 'F', [0x0A] = 'G', [0x0B] = 'H', [0x0C] = 'I', [0x0D] = 'J',
    [0x0E] = 'K', [0x0F] = 'L', [0x10] = 'M', [0x11] = 'N', [0x12] = 'O',
    [0x13] = 'P', [0x14] = 'Q', [0x15] = 'R', [0x16] = 'S', [0x17] = 'T',
    [0x18] = 'U', [0x19] = 'V', [0x1A] = 'W', [0x1B] = 'X', [0x1C] = 'Y',
    [0x1D] = 'Z', [0x2F] = '{', [0x30] = '}', [0x31] = '|', [0x33] = ':',
    [0x34] = '"', [0x35] = '~', [0x36] = '<', [0x37] = '>', [0x38] = '?',
    [0x2C] = ' ',
};

static bool hid_kbd_pressed(const usbhid_dev_t *h, uint8_t usage) {
    for (int i = 2; i < KBD_REPORT_LEN; i++)
        if (h->prev[i] == usage) return true;
    return false;
}

static void hid_kbd_report(usbhid_dev_t *h, const uint8_t *r) {
    uint8_t mods = r[0];
    bool shift = (mods & 0x22) != 0;

    for (int i = 2; i < KBD_REPORT_LEN; i++) {
        uint8_t u = h->prev[i];
        if (!u) continue;
        bool still = false;
        for (int j = 2; j < KBD_REPORT_LEN; j++)
            if (r[j] == u) still = true;
        if (!still && u < 128) {
            uint16_t lk = hid_usage_linuxkey[u];
            if (lk) input_push(INPUT_DEV_KBD, EV_KEY, lk, 0);
        }
    }

    for (int i = 2; i < KBD_REPORT_LEN; i++) {
        uint8_t u = r[i];
        if (!u || hid_kbd_pressed(h, u)) continue;
        if (u >= 128) continue;
        uint16_t lk = hid_usage_linuxkey[u];
        if (lk) input_push(INPUT_DEV_KBD, EV_KEY, lk, 1);
        if (!g_evdev_kbd_open) {
            char c = shift ? hid_ascii_shift[u] : hid_ascii[u];
            if (c) tty_putchar(c);
        }
    }
    memcpy(h->prev, r, KBD_REPORT_LEN);
}

static uint8_t g_mouse_prev_btn;

static void hid_mouse_report(const uint8_t *r) {
    int8_t dx = (int8_t) r[1];
    int8_t dy = (int8_t) r[2];
    int8_t dw = 0;
    if (r[0] || dx || dy) {
        if (dx) input_push(INPUT_DEV_MOUSE, EV_REL, REL_X, dx);
        if (dy) input_push(INPUT_DEV_MOUSE, EV_REL, REL_Y, -dy);
    }
    if (r[3]) dw = (int8_t) r[3];
    if (dw) input_push(INPUT_DEV_MOUSE, EV_REL, REL_WHEEL, dw);
    uint8_t btn = r[0];
    uint8_t chg = btn ^ g_mouse_prev_btn;
    if (chg & 1) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_LEFT, (btn & 1) ? 1 : 0);
    if (chg & 2) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_RIGHT, (btn & 2) ? 1 : 0);
    if (chg & 4) input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_MIDDLE, (btn & 4) ? 1 : 0);
    g_mouse_prev_btn = btn;
}

void usbhid_poll(void) {
    for (int i = 0; i < g_nhid; i++) {
        usbhid_dev_t *h = &g_hid[i];
        int actual = 0;
        int r = usb_interrupt_transfer(h->dev, h->ep_addr, &h->iface->eps[0].toggle, h->buf,
                                       h->mps, &actual);
        if (r == USB_STALL) {
            usb_clear_halt(h->dev, h->ep_addr);
            continue;
        }
        if (r || actual <= 0) continue;
        if (h->is_kbd) {
            if (actual >= KBD_REPORT_LEN) hid_kbd_report(h, h->buf);
        } else {
            if (actual >= 3) hid_mouse_report(h->buf);
        }
    }
}

void usbhid_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nhid >= USBHID_MAX_DEVS) return;
    if (iface->num_eps < 1) return;
    bool boot = iface->subclass == USB_SUBCLASS_BOOT;
    bool is_kbd = boot && iface->protocol == USB_PROTOCOL_KBD;
    bool is_mouse = boot && iface->protocol == USB_PROTOCOL_MOUSE;
    if (!is_kbd && !is_mouse) return;

    usb_endpoint_t *ep = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        if ((iface->eps[i].attributes & 3) == 3 && (iface->eps[i].addr & 0x80)) {
            ep = &iface->eps[i];
            break;
        }
    }
    if (!ep) ep = &iface->eps[0];

    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
                    USB_REQ_SET_PROTOCOL, 0, iface->number, NULL, 0, NULL);
    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE, USB_REQ_SET_IDLE,
                    0, iface->number, NULL, 0, NULL);

    usbhid_dev_t *h = &g_hid[g_nhid];
    memset(h, 0, sizeof(*h));
    h->dev = dev;
    h->iface = iface;
    h->ep_addr = ep->addr;
    h->mps = ep->max_packet;
    h->interval = ep->interval ? ep->interval : 10;
    h->is_kbd = is_kbd;
    if (h->mps < MOUSE_REPORT_LEN) h->mps = MOUSE_REPORT_LEN;

    uint64_t pages = (h->mps + PAGE_SIZE - 1) / PAGE_SIZE;
    h->buf_phys = (uint64_t) pmm_alloc_contiguous(pages);
    if (!h->buf_phys) return;
    h->buf = (uint8_t *) phys_to_virt(h->buf_phys);

    g_nhid++;
    log_info("USB HID: %s on addr %d ep 0x%02x mps=%d interval=%d",
             is_kbd ? "keyboard" : "mouse", dev->addr, h->ep_addr, h->mps, h->interval);
}

void usbhid_init(void) {
    g_nhid = 0;
    g_mouse_prev_btn = 0;
}
