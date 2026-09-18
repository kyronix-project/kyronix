#include "usb.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../netdev.h"

#define USBNET_MAX 2
#define USBNET_XFER 2048

#define AX88772_CMD_RXTX_WRITE 0x02
#define AX88772_CMD_RXTX_READ 0x03
#define AX88772_CMD_RXCTL_WRITE 0x0B
#define AX88772_CMD_NODEID_READ 0x13
#define AX88772_CMD_NODEID_WRITE 0x14
#define AX88772_CMD_PHY_SELECT 0x1C
#define AX88772_RXCTL_SO 0x0080
#define AX88772_RXCTL_AB 0x0008
#define AX88772_RXCTL_AM 0x0004
#define AX88772_RXCTL_PRO 0x0001

#define RTL8152_REQT_READ 0xC0
#define RTL8152_REQT_WRITE 0x40
#define RTL8152_REQ_GET_REGS 0x05
#define RTL8152_REQ_SET_REGS 0x05
#define RTL8152_PLA_BASE 0x0100
#define RTL8152_PLA_IDR 0x0100
#define RTL8152_PLA_RCR 0x0120
#define RTL8152_PLA_RXFIFO_FULL 0x0130
#define RTL8152_PLA_PHYSTATUS 0x0110
#define RTL8152_PLA_TXFIFO_EMPTY 0x0128
#define RTL8152_PLA_RXFIFO_EMPTY 0x012C
#define RTL8152_PLA_CR 0x012E
#define RTL8152_PLA_MSR 0x0112
#define RTL8152_PLA_CRWECR 0x0138
#define RTL8152_PLA_PHY_PWR 0x0114
#define RTL8152_PLA_MAC_PWR_CTRL 0x0128
#define RTL8152_PLA_MAC_PWR_CTRL2 0x012A
#define RTL8152_PLA_MAC_PWR_CTRL3 0x012C
#define RTL8152_PLA_MAC_PWR_CTRL4 0x012E
#define RTL8152_PLA_EXTRA_STATUS 0x0108
#define RTL8152_PLA_PHYAR 0x0118
#define RTL8152_PLA_LEDSEL 0x0132
#define RTL8152_PLA_LED_FEATURE 0x0134
#define RTL8152_PLA_PHYAR 0x0118
#define RTL8152_PLA_PHYAR 0x0118
#define RTL8152_PLA_EEE_ADV 0x0160
#define RTL8152_PLA_EEE_LPABLE 0x0162
#define RTL8152_PLA_PHY_CONFIG 0x0164
#define RTL8152_PLA_PHY_CONFIG2 0x0166
#define RTL8152_PLA_PHY_CONFIG3 0x0168
#define RTL8152_PLA_PHY_CONFIG4 0x016A
#define RTL8152_PLA_PHY_CONFIG5 0x016C
#define RTL8152_PLA_PHY_STATUS 0x016E
#define RTL8152_PLA_PHY_ANAR 0x0170
#define RTL8152_PLA_PHY_ANLPAR 0x0172
#define RTL8152_PLA_PHY_GBCR 0x0174
#define RTL8152_PLA_PHY_GBSR 0x0176
#define RTL8152_PLA_PHY_MACR 0x0178
#define RTL8152_PLA_PHY_MACSR 0x017A
#define RTL8152_PLA_PHY_EEE_TXIDLE 0x017C
#define RTL8152_PLA_PHY_EEE_RXIDLE 0x017E
#define RTL8152_PLA_PHY_MMD 0x0180
#define RTL8152_PLA_PHY_MMD_INDEX 0x0182
#define RTL8152_PLA_PHY_MMD_DATA 0x0184
#define RTL8152_PLA_PHY_MMD_CTRL 0x0186
#define RTL8152_PLA_PHY_MMD_CTRL2 0x0188
#define RTL8152_PLA_PHY_MMD_CTRL3 0x018A
#define RTL8152_PLA_PHY_MMD_CTRL4 0x018C
#define RTL8152_PLA_PHY_MMD_CTRL5 0x018E
#define RTL8152_PLA_PHY_MMD_CTRL6 0x0190
#define RTL8152_PLA_PHY_MMD_CTRL7 0x0192
#define RTL8152_PLA_PHY_MMD_CTRL8 0x0194
#define RTL8152_PLA_PHY_MMD_CTRL9 0x0196
#define RTL8152_PLA_PHY_MMD_CTRL10 0x0198
#define RTL8152_PLA_PHY_MMD_CTRL11 0x019A
#define RTL8152_PLA_PHY_MMD_CTRL12 0x019C
#define RTL8152_PLA_PHY_MMD_CTRL13 0x019E
#define RTL8152_PLA_PHY_MMD_CTRL14 0x01A0
#define RTL8152_PLA_PHY_MMD_CTRL15 0x01A2
#define RTL8152_PLA_PHY_MMD_CTRL16 0x01A4
#define RTL8152_PLA_PHY_MMD_CTRL17 0x01A6
#define RTL8152_PLA_PHY_MMD_CTRL18 0x01A8
#define RTL8152_PLA_PHY_MMD_CTRL19 0x01AA
#define RTL8152_PLA_PHY_MMD_CTRL20 0x01AC
#define RTL8152_PLA_PHY_MMD_CTRL21 0x01AE
#define RTL8152_PLA_PHY_MMD_CTRL22 0x01B0
#define RTL8152_PLA_PHY_MMD_CTRL23 0x01B2
#define RTL8152_PLA_PHY_MMD_CTRL24 0x01B4
#define RTL8152_PLA_PHY_MMD_CTRL25 0x01B6
#define RTL8152_PLA_PHY_MMD_CTRL26 0x01B8
#define RTL8152_PLA_PHY_MMD_CTRL27 0x01BA
#define RTL8152_PLA_PHY_MMD_CTRL28 0x01BC
#define RTL8152_PLA_PHY_MMD_CTRL29 0x01BE
#define RTL8152_PLA_PHY_MMD_CTRL30 0x01C0
#define RTL8152_PLA_PHY_MMD_CTRL31 0x01C2

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *xfer;
    uint64_t xfer_phys;
    bool is_rtl8152;
    netdev_t nd;
} usbnet_t;

static usbnet_t g_unet[USBNET_MAX];
static int g_nunet;

static int ax88772_vendor_cmd(usbnet_t *n, uint8_t cmd, uint16_t value, uint16_t index,
                              void *data, uint16_t len, bool read) {
    uint8_t reqtype = read ? (USB_REQTYPE_DIR_IN | 0x40) : 0x40;
    return usb_control_msg(n->dev, reqtype, cmd, value, index, data, len, NULL);
}

static int ax88772_read_mac(usbnet_t *n) {
    uint8_t mac[6];
    if (ax88772_vendor_cmd(n, AX88772_CMD_NODEID_READ, 0, 0, mac, 6, true) < 0) return -1;
    memcpy(n->nd.mac, mac, 6);
    return 0;
}

static int ax88772_init(usbnet_t *n) {
    uint16_t val = 0;
    ax88772_vendor_cmd(n, 0x1C, 0, 0, &val, 1, true);
    ax88772_vendor_cmd(n, 0x1C, 1, 0, NULL, 0, false);
    uint16_t rxctl = AX88772_RXCTL_SO | AX88772_RXCTL_AB | AX88772_RXCTL_AM;
    ax88772_vendor_cmd(n, AX88772_CMD_RXCTL_WRITE, rxctl, 0, NULL, 0, false);
    ax88772_vendor_cmd(n, AX88772_CMD_RXTX_WRITE, 0x01, 0, NULL, 0, false);
    return 0;
}

static int ax88772_send(usbnet_t *n, const uint8_t *frame, uint16_t len) {
    uint8_t *p = n->xfer;
    uint32_t hdr = (uint32_t) len | (((uint32_t) (~len) & 0xFFFF) << 16);
    memcpy(p, &hdr, 4);
    memcpy(p + 4, frame, len);
    int actual = 0;
    int ret = usb_bulk_transfer(n->dev, n->ep_out, n->toggle_out, p, (int) (4 + len), &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(n->dev, n->ep_out);
        ret = usb_bulk_transfer(n->dev, n->ep_out, n->toggle_out, p, (int) (4 + len), &actual);
    }
    return ret;
}

static int rtl8152_read_reg(usbnet_t *n, uint16_t addr, void *data, uint16_t len) {
    return usb_control_msg(n->dev, RTL8152_REQT_READ, RTL8152_REQ_GET_REGS, addr, 0, data, len,
                           NULL);
}

static int rtl8152_write_reg(usbnet_t *n, uint16_t addr, const void *data, uint16_t len) {
    return usb_control_msg(n->dev, RTL8152_REQT_WRITE, RTL8152_REQ_SET_REGS, addr, 0,
                           (void *) data, len, NULL);
}

static int rtl8152_read_mac(usbnet_t *n) {
    uint8_t mac[6];
    if (rtl8152_read_reg(n, RTL8152_PLA_IDR, mac, 6) < 0) return -1;
    memcpy(n->nd.mac, mac, 6);
    return 0;
}

static int rtl8152_init(usbnet_t *n) {
    uint8_t val8;
    uint16_t val16;
    uint32_t val32;

    rtl8152_read_reg(n, RTL8152_PLA_CR, &val8, 1);
    val8 |= 0x03;
    rtl8152_write_reg(n, RTL8152_PLA_CR, &val8, 1);

    rtl8152_read_reg(n, RTL8152_PLA_RCR, &val32, 4);
    val32 |= 0x00000002u | 0x00000008u | 0x00000004u;
    rtl8152_write_reg(n, RTL8152_PLA_RCR, &val32, 4);

    rtl8152_read_reg(n, RTL8152_PLA_MSR, &val8, 1);
    val8 |= 0x0C;
    rtl8152_write_reg(n, RTL8152_PLA_MSR, &val8, 1);

    rtl8152_read_reg(n, RTL8152_PLA_PHYSTATUS, &val16, 2);
    val16 |= 0x0001;
    rtl8152_write_reg(n, RTL8152_PLA_PHYSTATUS, &val16, 2);

    return 0;
}

static int rtl8152_send(usbnet_t *n, const uint8_t *frame, uint16_t len) {
    uint8_t *p = n->xfer;
    uint32_t opts1 = (uint32_t) len | (1u << 31) | (1u << 30) | (1u << 28);
    uint32_t opts2 = 0;
    memcpy(p, &opts1, 4);
    memcpy(p + 4, &opts2, 4);
    memcpy(p + 8, frame, len);
    int actual = 0;
    int ret = usb_bulk_transfer(n->dev, n->ep_out, n->toggle_out, p, (int) (8 + len), &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(n->dev, n->ep_out);
        ret = usb_bulk_transfer(n->dev, n->ep_out, n->toggle_out, p, (int) (8 + len), &actual);
    }
    return ret;
}

static int usbnet_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    usbnet_t *n = (usbnet_t *) nd->priv;
    return n->is_rtl8152 ? rtl8152_send(n, frame, len) : ax88772_send(n, frame, len);
}

static void usbnet_poll(netdev_t *nd) {
    usbnet_t *n = (usbnet_t *) nd->priv;
    for (int i = 0; i < 4; i++) {
        int actual = 0;
        int ret = usb_bulk_transfer(n->dev, n->ep_in, n->toggle_in, n->xfer, USBNET_XFER,
                                    &actual);
        if (ret == USB_STALL) {
            usb_clear_halt(n->dev, n->ep_in);
            continue;
        }
        if (ret || actual < 4) break;
        if (n->is_rtl8152) {
            if (actual < 8) continue;
            uint32_t rx_len = *(uint32_t *) (n->xfer + 4);
            if (rx_len < 14 || rx_len > (uint32_t) actual - 8) continue;
            netdev_receive(nd, n->xfer + 8, (uint16_t) (rx_len - 4));
        } else {
            uint16_t rx_len = n->xfer[0] | (n->xfer[1] << 8);
            if (rx_len < 14 || rx_len > (uint16_t) actual - 4) continue;
            netdev_receive(nd, n->xfer + 4, (uint16_t) (rx_len - 4));
        }
    }
}

void usbnet_asix_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nunet >= USBNET_MAX) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    usbnet_t *n = &g_unet[g_nunet];
    memset(n, 0, sizeof(*n));
    n->dev = dev;
    n->iface = iface;
    n->ep_in = ep_in->addr;
    n->ep_out = ep_out->addr;
    n->toggle_in = &ep_in->toggle;
    n->toggle_out = &ep_out->toggle;
    n->is_rtl8152 = false;

    n->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (USBNET_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!n->xfer_phys) return;
    n->xfer = (uint8_t *) phys_to_virt(n->xfer_phys);

    if (ax88772_read_mac(n) || ax88772_init(n)) {
        log_warn("usbnet: ASIX init failed");
        return;
    }

    snprintf(n->nd.name, NETDEV_NAME_MAX, "ax%d", g_nunet);
    n->nd.send = usbnet_send;
    n->nd.poll = usbnet_poll;
    n->nd.priv = n;
    netdev_register(&n->nd);
    log_info("usbnet: ASIX AX88772 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x", n->nd.mac[0],
             n->nd.mac[1], n->nd.mac[2], n->nd.mac[3], n->nd.mac[4], n->nd.mac[5]);
    g_nunet++;
}

void usbnet_rtl8152_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nunet >= USBNET_MAX) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    usbnet_t *n = &g_unet[g_nunet];
    memset(n, 0, sizeof(*n));
    n->dev = dev;
    n->iface = iface;
    n->ep_in = ep_in->addr;
    n->ep_out = ep_out->addr;
    n->toggle_in = &ep_in->toggle;
    n->toggle_out = &ep_out->toggle;
    n->is_rtl8152 = true;

    n->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (USBNET_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!n->xfer_phys) return;
    n->xfer = (uint8_t *) phys_to_virt(n->xfer_phys);

    if (rtl8152_read_mac(n) || rtl8152_init(n)) {
        log_warn("usbnet: RTL8152 init failed");
        return;
    }

    snprintf(n->nd.name, NETDEV_NAME_MAX, "rtl%d", g_nunet);
    n->nd.send = usbnet_send;
    n->nd.poll = usbnet_poll;
    n->nd.priv = n;
    netdev_register(&n->nd);
    log_info("usbnet: RTL8152/8153 ready, MAC %02x:%02x:%02x:%02x:%02x:%02x", n->nd.mac[0],
             n->nd.mac[1], n->nd.mac[2], n->nd.mac[3], n->nd.mac[4], n->nd.mac[5]);
    g_nunet++;
}
