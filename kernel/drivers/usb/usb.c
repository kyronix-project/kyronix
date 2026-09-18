#include "usb.h"
#include "../../arch/x86_64/cpu.h"
#include "../../arch/x86_64/pit.h"
#include "../../lib/log.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"

static usb_device_t g_usb_devices[USB_MAX_DEVICES];
static int g_usb_ndevs;
static usb_hc_t *g_hcs;
static bool g_usb_ready;

void usb_msleep(uint32_t ms) {
    uint64_t end = g_ticks + ms + 1u;
    while (g_ticks < end) cpu_relax();
}

void usb_hc_register(usb_hc_t *hc) {
    hc->next = g_hcs;
    g_hcs = hc;
    log_info("USB: registered host controller '%s' with %d ports", hc->name, hc->num_ports);
}

static usb_device_t *usb_alloc_device(void) {
    if (g_usb_ndevs >= USB_MAX_DEVICES) return NULL;
    usb_device_t *d = &g_usb_devices[g_usb_ndevs];
    memset(d, 0, sizeof(*d));
    d->slot = g_usb_ndevs;
    return d;
}

int usb_device_count(void) { return g_usb_ndevs; }
usb_device_t *usb_get_device(int idx) {
    if (idx < 0 || idx >= g_usb_ndevs) return NULL;
    return &g_usb_devices[idx];
}

int usb_control_msg(usb_device_t *dev, uint8_t reqtype, uint8_t request, uint16_t value,
                    uint16_t index, void *buf, int len, int *actual) {
    usb_setup_pkt_t setup;
    setup.bmRequestType = reqtype;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = (uint16_t) len;
    return dev->hc->ops->control(dev->hc, dev, &setup, buf, len, actual);
}

int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index, void *buf, int len) {
    return usb_control_msg(dev, USB_REQTYPE_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                           (uint16_t) ((type << 8) | index), 0, buf, len, NULL);
}

int usb_set_address(usb_device_t *dev, int addr) {
    return usb_control_msg(dev, 0, USB_REQ_SET_ADDRESS, (uint16_t) addr, 0, NULL, 0, NULL);
}

int usb_set_configuration(usb_device_t *dev, int config) {
    return usb_control_msg(dev, 0, USB_REQ_SET_CONFIGURATION, (uint16_t) config, 0, NULL, 0, NULL);
}

int usb_clear_halt(usb_device_t *dev, uint8_t ep_addr) {
    int r = usb_control_msg(dev, USB_REQTYPE_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                            USB_FEATURE_ENDPOINT_HALT, ep_addr, NULL, 0, NULL);
    for (int i = 0; i < dev->num_ifaces; i++)
        for (int e = 0; e < dev->ifaces[i].num_eps; e++)
            if (dev->ifaces[i].eps[e].addr == ep_addr) dev->ifaces[i].eps[e].toggle = 0;
    return r;
}

int usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle, void *buf, int len,
                      int *actual) {
    return dev->hc->ops->bulk(dev->hc, dev, ep_addr, toggle, buf, len, actual);
}

int usb_interrupt_transfer(usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle, void *buf,
                           int len, int *actual) {
    return dev->hc->ops->interrupt(dev->hc, dev, ep_addr, toggle, buf, len, actual);
}

static void usb_parse_config(usb_device_t *dev, const uint8_t *buf, int len) {
    int off = 0;
    usb_interface_t *cur = NULL;
    dev->num_ifaces = 0;
    while (off + 2 <= len && dev->num_ifaces < USB_MAX_INTERFACES) {
        uint8_t blen = buf[off];
        uint8_t btype = buf[off + 1];
        if (blen < 2 || off + blen > len) break;
        if (btype == USB_DT_INTERFACE && blen >= (int) sizeof(usb_interface_desc_t)) {
            const usb_interface_desc_t *id = (const usb_interface_desc_t *) (buf + off);
            cur = &dev->ifaces[dev->num_ifaces++];
            memset(cur, 0, sizeof(*cur));
            cur->number = id->bInterfaceNumber;
            cur->class_code = id->bInterfaceClass;
            cur->subclass = id->bInterfaceSubClass;
            cur->protocol = id->bInterfaceProtocol;
        } else if (btype == USB_DT_ENDPOINT && blen >= 7 && cur &&
                   cur->num_eps < USB_MAX_ENDPOINTS) {
            const usb_endpoint_desc_t *ed = (const usb_endpoint_desc_t *) (buf + off);
            usb_endpoint_t *ep = &cur->eps[cur->num_eps++];
            ep->addr = ed->bEndpointAddress;
            ep->attributes = ed->bmAttributes;
            ep->max_packet = ed->wMaxPacketSize;
            ep->interval = ed->bInterval;
            ep->toggle = 0;
        }
        off += blen;
    }
}

static void usb_dispatch_class(usb_device_t *dev, usb_interface_t *iface) {
    if (iface->class_code == USB_CLASS_HID) {
        usbhid_probe(dev, iface);
    } else if (iface->class_code == USB_CLASS_MASS_STORAGE &&
               iface->subclass == USB_SUBCLASS_SCSI && iface->protocol == USB_PROTOCOL_BOT) {
        usbms_probe(dev, iface);
    } else if (iface->class_code == 0x02 && iface->subclass == 0x02 &&
               iface->protocol == 0xFF) {
        usbrndis_probe(dev, iface);
    } else if (iface->class_code == 0x02 && iface->subclass == 0x06) {
        usbcdcecm_probe(dev, iface);
    } else if (iface->class_code == 0x02 && iface->subclass == 0x02 &&
               iface->protocol == 0x01) {
        usbcdcacm_probe(dev, iface);
    } else if (iface->class_code == 0x0A) {
        usbcdcecm_probe(dev, iface);
    } else if (iface->class_code == 0xFF) {
        if (dev->desc.idVendor == 0x0B95)
            usbnet_asix_probe(dev, iface);
        else if (dev->desc.idVendor == 0x0BDA) {
            if (dev->desc.idProduct == 0x8152 || dev->desc.idProduct == 0x8153 ||
                dev->desc.idProduct == 0x8156)
                usbnet_rtl8152_probe(dev, iface);
            else if (dev->desc.idProduct == 0x8179 || dev->desc.idProduct == 0x0179 ||
                     dev->desc.idProduct == 0x8178)
                rtl8188eu_probe(dev, iface);
        }
    }
}

static void usb_enumerate_port(usb_hc_t *hc, int port, usb_device_t *parent, int depth);

static void usb_hub_configure(usb_device_t *dev, int depth) {
    static const int hub_max_ports = USB_MAX_HUB_PORTS;
    int nports = hub_max_ports;
    if (dev->desc.bDeviceProtocol == 0 && dev->ifaces[0].num_eps == 0) nports = 4;
    for (int p = 1; p <= nports; p++) {
        uint16_t status = 0;
        if (usb_control_msg(dev, USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_CLASS |
                                       USB_REQTYPE_RECIP_OTHER,
                            USB_REQ_GET_STATUS, 0, (uint16_t) p, &status, 2, NULL) < 0)
            continue;
        if (!(status & 0x01u)) continue;
        if (usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_OTHER,
                            USB_REQ_SET_FEATURE, USB_FEATURE_PORT_RESET, (uint16_t) p, NULL, 0,
                            NULL) < 0)
            continue;
        usb_msleep(60);
        usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_OTHER,
                        USB_REQ_CLEAR_FEATURE, USB_FEATURE_C_PORT_CONNECTION, (uint16_t) p, NULL,
                        0, NULL);
        usb_control_msg(dev, USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_OTHER,
                        USB_REQ_CLEAR_FEATURE, USB_FEATURE_C_PORT_RESET, (uint16_t) p, NULL, 0,
                        NULL);
        if (usb_control_msg(dev, USB_REQTYPE_DIR_IN | USB_REQTYPE_TYPE_CLASS |
                                       USB_REQTYPE_RECIP_OTHER,
                            USB_REQ_GET_STATUS, 0, (uint16_t) p, &status, 2, NULL) < 0)
            continue;
        if (!(status & 0x01u) || !(status & 0x02u)) continue;
        if (depth < 4) usb_enumerate_port(dev->hc, p, dev, depth + 1);
    }
}

static void usb_enumerate_port(usb_hc_t *hc, int port, usb_device_t *parent, int depth) {
    int speed = hc->ops->reset_port(hc, port);
    if (speed < 0) return;
    usb_msleep(20);

    usb_device_t *dev = usb_alloc_device();
    if (!dev) return;
    dev->hc = hc;
    dev->port = port;
    dev->parent = parent;
    dev->addr = 0;
    dev->speed = speed;
    dev->max_packet0 = 8;

    uint8_t rawdesc[256];
    usb_device_desc_t *dd = (usb_device_desc_t *) rawdesc;
    if (usb_get_descriptor(dev, USB_DT_DEVICE, 0, rawdesc, 8) < 0) {
        log_warn("USB: port %d: failed to read device descriptor prefix", port);
        return;
    }
    dev->max_packet0 = dd->bMaxPacketSize0;
    if (dev->max_packet0 == 9) dev->max_packet0 = 512;
    if (dev->max_packet0 < 8) dev->max_packet0 = 8;

    if (hc->ops->reset_port(hc, port) < 0) return;
    usb_msleep(20);

    int addr = g_usb_ndevs + 1;
    if (addr > 0x7F) addr = 1;
    if (usb_set_address(dev, addr) < 0) {
        log_warn("USB: port %d: SET_ADDRESS failed", port);
        return;
    }
    usb_msleep(5);
    dev->addr = addr;

    if (usb_get_descriptor(dev, USB_DT_DEVICE, 0, rawdesc, sizeof(usb_device_desc_t)) < 0) {
        log_warn("USB: addr %d: full device descriptor failed", addr);
        return;
    }
    memcpy(&dev->desc, rawdesc, sizeof(usb_device_desc_t));

    log_info("USB: device %04x:%04x class=%02x/%02x/%02x speed=%d on %s port %d (addr %d)",
             dev->desc.idVendor, dev->desc.idProduct, dev->desc.bDeviceClass,
             dev->desc.bDeviceSubClass, dev->desc.bDeviceProtocol, dev->speed, hc->name, port,
             addr);

    if (usb_get_descriptor(dev, USB_DT_CONFIG, 0, rawdesc, sizeof(usb_config_desc_t)) < 0)
        return;
    usb_config_desc_t *cd = (usb_config_desc_t *) rawdesc;
    int total = cd->wTotalLength;
    int cfgval = cd->bConfigurationValue;
    if (total > (int) sizeof(rawdesc)) total = sizeof(rawdesc);
    if (usb_get_descriptor(dev, USB_DT_CONFIG, 0, rawdesc, total) < 0) return;
    usb_parse_config(dev, rawdesc, total);

    if (usb_set_configuration(dev, cfgval) < 0) {
        log_warn("USB: addr %d: SET_CONFIGURATION %d failed", addr, cfgval);
        return;
    }
    dev->configured = true;
    g_usb_ndevs++;

    if (dev->desc.bDeviceClass == USB_CLASS_HUB) {
        usb_hub_configure(dev, depth);
        return;
    }

    for (int i = 0; i < dev->num_ifaces; i++) {
        usb_interface_t *iface = &dev->ifaces[i];
        log_info("USB:   iface %d class=%02x/%02x/%02x eps=%d", iface->number, iface->class_code,
                 iface->subclass, iface->protocol, iface->num_eps);
        usb_dispatch_class(dev, iface);
    }
}

void usb_enumerate_all(void) {
    for (usb_hc_t *hc = g_hcs; hc; hc = hc->next) {
        for (int p = 1; p <= hc->num_ports; p++) usb_enumerate_port(hc, p, NULL, 0);
    }
}

void usb_init(void) {
    g_usb_ndevs = 0;
    g_hcs = NULL;

    xhci_init();
    ehci_init();
    ohci_init();
    uhci_init();

    if (!g_hcs) {
        log_info("USB: no host controllers found");
        return;
    }

    usb_msleep(100);
    usb_enumerate_all();
    g_usb_ready = true;
    log_info("USB: %d device(s) enumerated", g_usb_ndevs);
}

bool usb_ready(void) { return g_usb_ready; }
