#include "usb.h"
#include "../../arch/x86_64/pit.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../netdev.h"

#define CDC_ACM_MAX 2
#define CDC_ACM_XFER 2048
#define ACM_REQ_SEND_ENCAPSULATED 0x00
#define ACM_REQ_SET_LINE_CODING 0x20
#define ACM_REQ_SET_CONTROL_LINE_STATE 0x22

typedef struct PACKED {
    uint32_t dwDTERate;
    uint8_t bCharFormat;
    uint8_t bParityType;
    uint8_t bDataBits;
} acm_line_coding_t;

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *xfer;
    uint64_t xfer_phys;
    uint8_t rx_buf[512];
    int rx_len;
    bool ppp_up;
    netdev_t nd;
} usbcdcacm_t;

static usbcdcacm_t g_acm[CDC_ACM_MAX];
static int g_nacm;

static int acm_send_bytes(usbcdcacm_t *a, const uint8_t *data, int len) {
    int actual = 0;
    int ret = usb_bulk_transfer(a->dev, a->ep_out, a->toggle_out, (void *) data, len, &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(a->dev, a->ep_out);
        ret = usb_bulk_transfer(a->dev, a->ep_out, a->toggle_out, (void *) data, len, &actual);
    }
    return ret == 0 ? actual : ret;
}

static int acm_recv_bytes(usbcdcacm_t *a, uint8_t *buf, int maxlen, int timeout_ms) {
    uint64_t end = g_ticks + (uint64_t) timeout_ms + 1u;
    while (g_ticks < end) {
        int actual = 0;
        int ret = usb_bulk_transfer(a->dev, a->ep_in, a->toggle_in, a->xfer, CDC_ACM_XFER,
                                    &actual);
        if (ret == USB_STALL) {
            usb_clear_halt(a->dev, a->ep_in);
            continue;
        }
        if (ret == 0 && actual > 0) {
            int take = actual < maxlen ? actual : maxlen;
            memcpy(buf, a->xfer, take);
            return take;
        }
        cpu_relax();
    }
    return 0;
}

static int acm_at_cmd(usbcdcacm_t *a, const char *cmd, const char *expect, int timeout_ms) {
    char buf[128];
    int n = 0;
    while (cmd[n] && n < 120) {
        buf[n] = cmd[n];
        n++;
    }
    buf[n++] = '\r';
    buf[n++] = '\n';
    acm_send_bytes(a, (const uint8_t *) buf, n);

    uint64_t end = g_ticks + (uint64_t) timeout_ms + 1u;
    char resp[256];
    int rlen = 0;
    while (g_ticks < end) {
        int got = acm_recv_bytes(a, (uint8_t *) resp + rlen, (int) sizeof(resp) - rlen - 1, 100);
        if (got > 0) {
            rlen += got;
            resp[rlen] = 0;
            if (strstr(resp, expect)) return 0;
            if (strstr(resp, "ERROR")) return -1;
        }
    }
    return -1;
}

static uint8_t ppp_escape(uint8_t c) { return c ^ 0x20; }

static int ppp_send_frame(usbcdcacm_t *a, const uint8_t *data, int len) {
    uint8_t *out = a->xfer;
    int pos = 0;
    out[pos++] = 0x7E;
    out[pos++] = 0xFF;
    out[pos++] = 0x03;
    for (int i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c < 0x20 || c == 0x7E || c == 0x7D) {
            out[pos++] = 0x7D;
            out[pos++] = ppp_escape(c);
        } else {
            out[pos++] = c;
        }
    }
    uint16_t fcs = 0xFFFF;
    for (int i = 0; i < len + 2; i++) {
        uint8_t b = (i < 2) ? (i == 0 ? 0xFF : 0x03) : data[i - 2];
        fcs ^= b;
        for (int j = 0; j < 8; j++) fcs = (fcs >> 1) ^ (0x8408 & -(int) (fcs & 1));
    }
    fcs = ~fcs;
    uint8_t f1 = (uint8_t) fcs, f2 = (uint8_t) (fcs >> 8);
    if (f1 < 0x20 || f1 == 0x7E || f1 == 0x7D) {
        out[pos++] = 0x7D;
        out[pos++] = ppp_escape(f1);
    } else out[pos++] = f1;
    if (f2 < 0x20 || f2 == 0x7E || f2 == 0x7D) {
        out[pos++] = 0x7D;
        out[pos++] = ppp_escape(f2);
    } else out[pos++] = f2;
    out[pos++] = 0x7E;
    return acm_send_bytes(a, out, pos) == pos ? 0 : -1;
}

static int ppp_lcp_config_req(usbcdcacm_t *a) {
    uint8_t pkt[] = {0xC0, 0x21, 0x01, 0x00, 0x00, 0x04};
    return ppp_send_frame(a, pkt, sizeof(pkt));
}

static int ppp_ipcp_config_req(usbcdcacm_t *a) {
    uint8_t pkt[] = {0x80, 0x21, 0x01, 0x01, 0x00, 0x0A, 0x03, 0x06, 0x00, 0x00, 0x00, 0x00};
    return ppp_send_frame(a, pkt, sizeof(pkt));
}

static int usbcdcacm_ppp_dial(usbcdcacm_t *a) {
    log_info("cdc-acm: dialing PPP...");
    if (acm_at_cmd(a, "AT", "OK", 2000)) {
        log_warn("cdc-acm: modem not responding to AT");
        return -1;
    }
    acm_at_cmd(a, "AT+CGDCONT=1,\"IP\",\"internet\"", "OK", 3000);
    if (acm_at_cmd(a, "ATDT*99#", "CONNECT", 15000)) {
        log_warn("cdc-acm: dial failed");
        return -1;
    }
    usb_msleep(500);
    if (ppp_lcp_config_req(a)) return -1;
    usb_msleep(500);
    if (ppp_ipcp_config_req(a)) return -1;
    a->ppp_up = true;
    log_info("cdc-acm: PPP up (raw dial, no full LCP negotiation)");
    return 0;
}

static int usbcdcacm_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    usbcdcacm_t *a = (usbcdcacm_t *) nd->priv;
    if (!a->ppp_up) return -1;
    uint8_t ip_pkt[1600];
    memcpy(ip_pkt, frame + 14, len - 14);
    return ppp_send_frame(a, ip_pkt, (int) (len - 14));
}

static void usbcdcacm_poll(netdev_t *nd) {
    usbcdcacm_t *a = (usbcdcacm_t *) nd->priv;
    if (!a->ppp_up) return;
    uint8_t buf[CDC_ACM_XFER];
    int got = acm_recv_bytes(a, buf, sizeof(buf), 1);
    if (got <= 0) return;
    for (int i = 0; i + 4 < got; i++) {
        if (buf[i] == 0x7E && buf[i + 1] == 0xFF && buf[i + 2] == 0x03) {
            int end = i + 3;
            while (end < got && buf[end] != 0x7E) end++;
            if (end - i - 4 >= 20) {
                uint8_t *ip = buf + i + 3;
                int ip_len = end - i - 5;
                uint8_t eth[1600];
                memset(eth, 0, 6);
                memcpy(eth + 6, nd->mac, 6);
                eth[12] = 0x08;
                eth[13] = 0x00;
                memcpy(eth + 14, ip, ip_len);
                netdev_receive(nd, eth, (uint16_t) (14 + ip_len));
            }
            break;
        }
    }
}

void usbcdcacm_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nacm >= CDC_ACM_MAX) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    usbcdcacm_t *a = &g_acm[g_nacm];
    memset(a, 0, sizeof(*a));
    a->dev = dev;
    a->iface = iface;
    a->ep_in = ep_in->addr;
    a->ep_out = ep_out->addr;
    a->toggle_in = &ep_in->toggle;
    a->toggle_out = &ep_out->toggle;

    a->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (CDC_ACM_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!a->xfer_phys) return;
    a->xfer = (uint8_t *) phys_to_virt(a->xfer_phys);

    acm_line_coding_t lc = {115200, 0, 0, 8};
    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
                    ACM_REQ_SET_LINE_CODING, 0, iface->number, &lc, sizeof(lc), NULL);
    usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE,
                    ACM_REQ_SET_CONTROL_LINE_STATE, 0x03, iface->number, NULL, 0, NULL);

    a->nd.mac[0] = 0x02;
    a->nd.mac[1] = 0x41;
    a->nd.mac[2] = 0x43;
    a->nd.mac[3] = 0x4D;
    a->nd.mac[4] = (uint8_t) g_nacm;
    a->nd.mac[5] = 0x01;

    snprintf(a->nd.name, NETDEV_NAME_MAX, "ppp%d", g_nacm);
    a->nd.send = usbcdcacm_send;
    a->nd.poll = usbcdcacm_poll;
    a->nd.priv = a;
    netdev_register(&a->nd);

    usbcdcacm_ppp_dial(a);
    g_nacm++;
}
