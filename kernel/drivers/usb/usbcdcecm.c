#include "usb.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../netdev.h"

#define CDC_ECM_MAX 2
#define CDC_ECM_XFER 2048

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *xfer;
    uint64_t xfer_phys;
    netdev_t nd;
} usbcdcecm_t;

static usbcdcecm_t g_ecm[CDC_ECM_MAX];
static int g_necm;

static int usbcdcecm_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    usbcdcecm_t *e = (usbcdcecm_t *) nd->priv;
    memcpy(e->xfer, frame, len);
    int actual = 0;
    int ret = usb_bulk_transfer(e->dev, e->ep_out, e->toggle_out, e->xfer, len, &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(e->dev, e->ep_out);
        ret = usb_bulk_transfer(e->dev, e->ep_out, e->toggle_out, e->xfer, len, &actual);
    }
    if (ret == 0 && (len % 64) == 0) {
        usb_bulk_transfer(e->dev, e->ep_out, e->toggle_out, e->xfer, 0, &actual);
    }
    return ret;
}

static void usbcdcecm_poll(netdev_t *nd) {
    usbcdcecm_t *e = (usbcdcecm_t *) nd->priv;
    for (int i = 0; i < 4; i++) {
        int actual = 0;
        int ret = usb_bulk_transfer(e->dev, e->ep_in, e->toggle_in, e->xfer, CDC_ECM_XFER,
                                    &actual);
        if (ret == USB_STALL) {
            usb_clear_halt(e->dev, e->ep_in);
            continue;
        }
        if (ret || actual < 14) break;
        netdev_receive(nd, e->xfer, (uint16_t) actual);
    }
}

static uint8_t ecm_hex_nibble(uint8_t c) {
    if (c >= '0' && c <= '9') return (uint8_t) (c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t) (c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t) (c - 'a' + 10);
    return 0;
}

static int usbcdcecm_get_mac(usbcdcecm_t *e, uint8_t imac_idx) {
    if (!imac_idx) return -1;
    uint8_t desc[256];
    int r = usb_get_descriptor(e->dev, USB_DT_STRING, imac_idx, desc, sizeof(desc));
    if (r < 0 || desc[1] != USB_DT_STRING) return -1;
    int chars = (desc[0] - 2) / 2;
    if (chars < 12) return -1;
    for (int i = 0; i < 6; i++) {
        uint8_t hi = desc[2 + i * 4];
        uint8_t lo = desc[4 + i * 4];
        e->nd.mac[i] = (uint8_t) ((ecm_hex_nibble(hi) << 4) | ecm_hex_nibble(lo));
    }
    return 0;
}

void usbcdcecm_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_necm >= CDC_ECM_MAX) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *ep = &iface->eps[i];
        if ((ep->attributes & 3) != 2) continue;
        if (ep->addr & 0x80) ep_in = ep;
        else ep_out = ep;
    }
    if (!ep_in || !ep_out) return;

    usbcdcecm_t *e = &g_ecm[g_necm];
    memset(e, 0, sizeof(*e));
    e->dev = dev;
    e->iface = iface;
    e->ep_in = ep_in->addr;
    e->ep_out = ep_out->addr;
    e->toggle_in = &ep_in->toggle;
    e->toggle_out = &ep_out->toggle;

    e->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (CDC_ECM_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!e->xfer_phys) return;
    e->xfer = (uint8_t *) phys_to_virt(e->xfer_phys);

    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE, 0x22, 0, 0,
                    NULL, 0, NULL);
    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE, 0x22, 0x01, 0,
                    NULL, 0, NULL);

    if (usbcdcecm_get_mac(e, iface->eps[0].addr & 0x7F)) {
        e->nd.mac[0] = 0x02;
        e->nd.mac[1] = 0x43;
        e->nd.mac[2] = 0x44;
        e->nd.mac[3] = 0x45;
        e->nd.mac[4] = 0x43;
        e->nd.mac[5] = 0x4D;
    }

    snprintf(e->nd.name, NETDEV_NAME_MAX, "uecm%d", g_necm);
    e->nd.send = usbcdcecm_send;
    e->nd.poll = usbcdcecm_poll;
    e->nd.priv = e;
    netdev_register(&e->nd);
    log_info("cdc-ecm: ready, MAC %02x:%02x:%02x:%02x:%02x:%02x", e->nd.mac[0], e->nd.mac[1],
             e->nd.mac[2], e->nd.mac[3], e->nd.mac[4], e->nd.mac[5]);
    g_necm++;
}
