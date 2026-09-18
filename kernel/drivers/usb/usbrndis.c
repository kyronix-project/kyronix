#include "usb.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../netdev.h"

#define RNDIS_MSG_COMPLETION 0x80000000u
#define RNDIS_MSG_PACKET 0x00000001u
#define RNDIS_MSG_INIT 0x00000002u
#define RNDIS_MSG_INIT_CMPLT (RNDIS_MSG_COMPLETION | RNDIS_MSG_INIT)
#define RNDIS_MSG_HALT 0x00000003u
#define RNDIS_MSG_QUERY 0x00000004u
#define RNDIS_MSG_QUERY_CMPLT (RNDIS_MSG_COMPLETION | RNDIS_MSG_QUERY)
#define RNDIS_MSG_SET 0x00000005u
#define RNDIS_MSG_SET_CMPLT (RNDIS_MSG_COMPLETION | RNDIS_MSG_SET)
#define RNDIS_MSG_RESET 0x00000006u
#define RNDIS_MSG_RESET_CMPLT (RNDIS_MSG_COMPLETION | RNDIS_MSG_RESET)
#define RNDIS_MSG_INDICATE 0x00000007u
#define RNDIS_MSG_KEEPALIVE 0x00000008u
#define RNDIS_MSG_KEEPALIVE_CMPLT (RNDIS_MSG_COMPLETION | RNDIS_MSG_KEEPALIVE)

#define RNDIS_OID_802_3_PERMANENT_ADDRESS 0x01010101u
#define RNDIS_OID_802_3_CURRENT_ADDRESS 0x01010102u
#define RNDIS_OID_GEN_CURRENT_PACKET_FILTER 0x0001010Eu
#define RNDIS_OID_GEN_MAXIMUM_FRAME_SIZE 0x00010106u
#define RNDIS_OID_GEN_LINK_SPEED 0x00010107u
#define RNDIS_OID_GEN_MEDIA_CONNECT_STATUS 0x00010114u

#define RNDIS_PACKET_FILTER_DIRECTED 0x00000001u
#define RNDIS_PACKET_FILTER_MULTICAST 0x00000002u
#define RNDIS_PACKET_FILTER_ALL_MULTICAST 0x00000004u
#define RNDIS_PACKET_FILTER_BROADCAST 0x00000008u
#define RNDIS_PACKET_FILTER_PROMISCUOUS 0x00000020u

#define RNDIS_CONTROL_TIMEOUT 5000
#define RNDIS_MAX_XFER 4096

typedef struct PACKED {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t status;
} rndis_msg_hdr_t;

typedef struct PACKED {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t major_ver;
    uint32_t minor_ver;
    uint32_t max_xfer_size;
} rndis_init_msg_t;

typedef struct PACKED {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t request_id;
    uint32_t oid;
    uint32_t info_buf_len;
    uint32_t info_buf_offset;
    uint32_t device_vc_handle;
} rndis_query_msg_t;

typedef struct PACKED {
    uint32_t msg_type;
    uint32_t msg_len;
    uint32_t data_offset;
    uint32_t data_len;
    uint32_t oob_data_offset;
    uint32_t oob_data_len;
    uint32_t num_oob;
    uint32_t packet_data_offset;
    uint32_t packet_data_len;
    uint32_t vc_handle;
    uint32_t reserved;
} rndis_packet_msg_t;

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *xfer;
    uint64_t xfer_phys;
    uint32_t request_id;
    netdev_t nd;
} usbrndis_t;

static usbrndis_t g_rndis[2];
static int g_nrndis;

static int rndis_control(usbrndis_t *r, uint32_t msg_type, const void *payload, int payload_len,
                         void *resp, int *resp_len) {
    rndis_msg_hdr_t *hdr = (rndis_msg_hdr_t *) r->xfer;
    memset(hdr, 0, 64);
    hdr->msg_type = msg_type;
    hdr->msg_len = (uint32_t) (sizeof(rndis_msg_hdr_t) + payload_len);
    hdr->request_id = ++r->request_id;

    if (payload_len > 0 && payload) memcpy(r->xfer + sizeof(rndis_msg_hdr_t), payload, payload_len);

    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer,
                                (int) hdr->msg_len, &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(r->dev, r->ep_out);
        ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer,
                                (int) hdr->msg_len, &actual);
    }
    if (ret || actual < (int) sizeof(rndis_msg_hdr_t)) return -1;

    usb_msleep(10);
    ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER, &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(r->dev, r->ep_in);
        ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER, &actual);
    }
    if (ret || actual < (int) sizeof(rndis_msg_hdr_t)) return -1;

    rndis_msg_hdr_t *rh = (rndis_msg_hdr_t *) r->xfer;
    if (rh->status != 0) return -1;
    if (resp && resp_len) {
        *resp_len = (int) rh->msg_len - (int) sizeof(rndis_msg_hdr_t);
        if (*resp_len > 0) memcpy(resp, r->xfer + sizeof(rndis_msg_hdr_t), *resp_len);
    }
    return 0;
}

static int rndis_init(usbrndis_t *r) {
    rndis_init_msg_t msg = {0};
    msg.msg_type = RNDIS_MSG_INIT;
    msg.msg_len = sizeof(msg);
    msg.request_id = ++r->request_id;
    msg.major_ver = 1;
    msg.minor_ver = 0;
    msg.max_xfer_size = RNDIS_MAX_XFER;

    memcpy(r->xfer, &msg, sizeof(msg));
    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer, sizeof(msg), &actual);
    if (ret) return ret;
    usb_msleep(50);
    ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER, &actual);
    if (ret || actual < 16) return -1;
    rndis_msg_hdr_t *rh = (rndis_msg_hdr_t *) r->xfer;
    return rh->status == 0 ? 0 : -1;
}

static int rndis_query_oid(usbrndis_t *r, uint32_t oid, void *out, int out_len) {
    rndis_query_msg_t q = {0};
    q.msg_type = RNDIS_MSG_QUERY;
    q.msg_len = sizeof(q);
    q.request_id = ++r->request_id;
    q.oid = oid;

    memcpy(r->xfer, &q, sizeof(q));
    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer, sizeof(q), &actual);
    if (ret) return ret;
    usb_msleep(10);
    ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER, &actual);
    if (ret || actual < 24) return -1;
    rndis_msg_hdr_t *rh = (rndis_msg_hdr_t *) r->xfer;
    if (rh->status != 0) return -1;
    uint32_t off = *(uint32_t *) (r->xfer + 20);
    uint32_t len = *(uint32_t *) (r->xfer + 16);
    if (len > (uint32_t) out_len) len = (uint32_t) out_len;
    memcpy(out, r->xfer + 24 + off - 8, len);
    return (int) len;
}

static int rndis_set_oid(usbrndis_t *r, uint32_t oid, const void *data, int data_len) {
    rndis_query_msg_t q = {0};
    q.msg_type = RNDIS_MSG_SET;
    q.msg_len = sizeof(q) + (uint32_t) data_len;
    q.request_id = ++r->request_id;
    q.oid = oid;
    q.info_buf_len = (uint32_t) data_len;
    q.info_buf_offset = (uint32_t) (sizeof(q) - 8);

    memcpy(r->xfer, &q, sizeof(q));
    memcpy(r->xfer + sizeof(q), data, data_len);
    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer, (int) q.msg_len, &actual);
    if (ret) return ret;
    usb_msleep(10);
    ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER, &actual);
    if (ret || actual < 16) return -1;
    rndis_msg_hdr_t *rh = (rndis_msg_hdr_t *) r->xfer;
    return rh->status == 0 ? 0 : -1;
}

static int usbrndis_send(netdev_t *nd, const uint8_t *frame, uint16_t len) {
    usbrndis_t *r = (usbrndis_t *) nd->priv;
    rndis_packet_msg_t *pkt = (rndis_packet_msg_t *) r->xfer;
    memset(pkt, 0, sizeof(*pkt));
    pkt->msg_type = RNDIS_MSG_PACKET;
    pkt->msg_len = (uint32_t) (sizeof(rndis_packet_msg_t) + len);
    pkt->data_offset = (uint32_t) (sizeof(rndis_packet_msg_t) - 8);
    pkt->data_len = len;
    memcpy(r->xfer + sizeof(rndis_packet_msg_t), frame, len);
    int actual = 0;
    int ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer, (int) pkt->msg_len,
                                &actual);
    if (ret == USB_STALL) {
        usb_clear_halt(r->dev, r->ep_out);
        ret = usb_bulk_transfer(r->dev, r->ep_out, r->toggle_out, r->xfer, (int) pkt->msg_len,
                                &actual);
    }
    return ret;
}

static void usbrndis_poll(netdev_t *nd) {
    usbrndis_t *r = (usbrndis_t *) nd->priv;
    for (int i = 0; i < 4; i++) {
        int actual = 0;
        int ret = usb_bulk_transfer(r->dev, r->ep_in, r->toggle_in, r->xfer, RNDIS_MAX_XFER,
                                    &actual);
        if (ret == USB_STALL) {
            usb_clear_halt(r->dev, r->ep_in);
            continue;
        }
        if (ret || actual < (int) sizeof(rndis_packet_msg_t)) break;
        rndis_packet_msg_t *pkt = (rndis_packet_msg_t *) r->xfer;
        if (pkt->msg_type != RNDIS_MSG_PACKET) continue;
        uint32_t data_off = pkt->data_offset + 8;
        uint32_t data_len = pkt->data_len;
        if (data_off + data_len > (uint32_t) actual) continue;
        if (data_len >= 14 && data_len < 1600)
            netdev_receive(nd, r->xfer + data_off, (uint16_t) data_len);
    }
}

void usbrndis_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nrndis >= 2) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    usbrndis_t *r = &g_rndis[g_nrndis];
    memset(r, 0, sizeof(*r));
    r->dev = dev;
    r->iface = iface;
    r->ep_in = ep_in->addr;
    r->ep_out = ep_out->addr;
    r->toggle_in = &ep_in->toggle;
    r->toggle_out = &ep_out->toggle;

    r->xfer_phys = (uint64_t) pmm_alloc_contiguous(
        (RNDIS_MAX_XFER + PAGE_SIZE - 1) / PAGE_SIZE);
    if (!r->xfer_phys) return;
    r->xfer = (uint8_t *) phys_to_virt(r->xfer_phys);

    if (rndis_init(r)) {
        log_warn("rndis: init failed");
        return;
    }

    uint32_t filter = RNDIS_PACKET_FILTER_DIRECTED | RNDIS_PACKET_FILTER_BROADCAST |
                      RNDIS_PACKET_FILTER_ALL_MULTICAST;
    if (rndis_set_oid(r, RNDIS_OID_GEN_CURRENT_PACKET_FILTER, &filter, 4)) {
        log_warn("rndis: set packet filter failed");
        return;
    }

    uint8_t mac[6] = {0};
    if (rndis_query_oid(r, RNDIS_OID_802_3_PERMANENT_ADDRESS, mac, 6) < 6) {
        rndis_query_oid(r, RNDIS_OID_802_3_CURRENT_ADDRESS, mac, 6);
    }
    memcpy(r->nd.mac, mac, 6);
    if (!(mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5])) {
        mac[0] = 0x02;
        mac[1] = 0x52;
        mac[2] = 0x4E;
        mac[3] = 0x44;
        mac[4] = 0x49;
        mac[5] = 0x53;
        memcpy(r->nd.mac, mac, 6);
    }

    snprintf(r->nd.name, NETDEV_NAME_MAX, "usb%d", g_nrndis);
    r->nd.send = usbrndis_send;
    r->nd.poll = usbrndis_poll;
    r->nd.priv = r;
    netdev_register(&r->nd);
    log_info("rndis: tethering device ready, MAC %02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[2], mac[3], mac[4], mac[5]);
    g_nrndis++;
}
