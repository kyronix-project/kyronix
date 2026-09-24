#include "../usb_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../xhci.h"

#define XHCI_MAX_HUBS 4

#define HUB_FEAT_PORT_RESET            4
#define HUB_FEAT_PORT_POWER            8
#define HUB_FEAT_C_PORT_CONNECTION    16
#define HUB_FEAT_C_PORT_RESET         20
#define HUB_FEAT_BH_PORT_RESET        28
#define HUB_FEAT_C_BH_PORT_RESET      29

#define HUB_PS_CONNECTION    (1u << 0)
#define HUB_PS_ENABLE        (1u << 1)
#define HUB_PS_RESET         (1u << 4)
#define HUB_PS_POWER         (1u << 8)
#define HUB_PS_LOW_SPEED     (1u << 9)
#define HUB_PS_HIGH_SPEED    (1u << 10)
#define HUB_PC_CONNECTION    (1u << 16)
#define HUB_PC_RESET         (1u << 20)

typedef struct {
    xhci_controller_t *ctl;
    uint8_t  slot_id;
    xhci_topology_t topo;
    xhci_trb_t *ep0_ring;
    uintptr_t   ep0_phys;
    uint16_t    ep0_enq;
    uint8_t     ep0_cyc;
    uint8_t  n_ports;
    uint8_t  pwr_on_2ms;
    bool     active;
} xhci_hub_t;

static xhci_hub_t g_hubs[XHCI_MAX_HUBS];
static int        g_hub_count = 0;

static int hub_get_descriptor(xhci_hub_t *h, uint8_t desc_type,
                              uint16_t length, uint8_t *out)
{
    uint16_t wValue = (uint16_t)desc_type << 8;
    return xhci_control_xfer(h->ctl, h->slot_id, h->ep0_ring, h->ep0_phys,
                             &h->ep0_enq, &h->ep0_cyc,
                             0xA0, USB_GET_DESCRIPTOR, wValue, 0, length, out);
}

static int hub_get_port_status(xhci_hub_t *h, uint8_t port, uint32_t *out_status) {
    uint8_t buf[4] = {0};
    int r = xhci_control_xfer(h->ctl, h->slot_id, h->ep0_ring, h->ep0_phys,
                              &h->ep0_enq, &h->ep0_cyc,
                              0xA3, 0, 0, port, 4, buf);
    if (r < 0) return r;
    *out_status = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8)
                | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return 0;
}

static int hub_set_port_feature(xhci_hub_t *h, uint8_t port, uint16_t feat) {
    return xhci_control_xfer(h->ctl, h->slot_id, h->ep0_ring, h->ep0_phys,
                             &h->ep0_enq, &h->ep0_cyc,
                             0x23, 3, feat, port, 0, NULL);
}

static int hub_clear_port_feature(xhci_hub_t *h, uint8_t port, uint16_t feat) {
    return xhci_control_xfer(h->ctl, h->slot_id, h->ep0_ring, h->ep0_phys,
                             &h->ep0_enq, &h->ep0_cyc,
                             0x23, 1, feat, port, 0, NULL);
}

static uint8_t hub_speed_from_status(uint32_t status, uint8_t hub_speed) {
    if (hub_speed >= 4) return 4;
    if (status & HUB_PS_LOW_SPEED)  return 2;
    if (status & HUB_PS_HIGH_SPEED) return 3;
    return 1;
}

static int hub_reset_enumerate_port(xhci_hub_t *h, uint8_t p) {
    uint32_t st = 0;
    if (hub_get_port_status(h, p, &st) < 0) return -EIO;
    if (!(st & HUB_PS_CONNECTION)) return -ENODEV;

    hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_CONNECTION);

    uint16_t reset_feat = (h->topo.speed >= 4)
                        ? HUB_FEAT_BH_PORT_RESET
                        : HUB_FEAT_PORT_RESET;
    if (hub_set_port_feature(h, p, reset_feat) < 0) return -EIO;

    bool reset_done = false;
    for (int j = 0; j < 200; j++) {
        usb_msleep(5);
        if (hub_get_port_status(h, p, &st) < 0) break;
        uint32_t change_bit = (h->topo.speed >= 4) ? (1u << 21) : HUB_PC_RESET;
        if (st & change_bit) { reset_done = true; break; }
    }
    if (!reset_done) {
        serial_printf("[xhci-hub] slot %u port %u: reset timeout\n", h->slot_id, p);
        return -EIO;
    }
    hub_clear_port_feature(h, p,
        (h->topo.speed >= 4) ? HUB_FEAT_C_BH_PORT_RESET : HUB_FEAT_C_PORT_RESET);

    if (hub_get_port_status(h, p, &st) < 0) return -EIO;
    if (!(st & HUB_PS_ENABLE)) {
        serial_printf("[xhci-hub] slot %u port %u: not enabled (status=0x%x)\n",
                      h->slot_id, p, st);
        return -EIO;
    }

    uint8_t child_speed = hub_speed_from_status(st, h->topo.speed);
    uint32_t shift = (uint32_t)h->topo.depth * 4;
    xhci_topology_t child = {
        .speed         = child_speed,
        .root_hub_port = h->topo.root_hub_port,
        .route_string  = h->topo.route_string | ((uint32_t)(p & 0xF) << shift),
        .parent_slot   = (child_speed <= 2) ? h->slot_id : 0,
        .parent_port   = (child_speed <= 2) ? p          : 0,
        .depth         = (uint8_t)(h->topo.depth + 1),
    };
    char label[48];
    snprintf(label, sizeof(label), "hub %u port %u", h->slot_id, p);
    enumerate_device(h->ctl, &child, label);
    return 0;
}

static int hub_query_descriptor(xhci_hub_t *h) {
    uint8_t hdr[16] = {0};
    uint16_t hub_desc_type = (h->topo.speed >= 4) ? 0x2A : 0x29;
    if (hub_get_descriptor(h, (uint8_t)hub_desc_type, 8, hdr) < 0) {
        serial_printf("[xhci-hub] slot %u: GET_HUB_DESCRIPTOR failed\n", h->slot_id);
        return -EIO;
    }
    h->n_ports    = hdr[2];
    h->pwr_on_2ms = hdr[5];
    serial_printf("[xhci-hub] slot %u: %u downstream ports, power-on delay %u ms\n",
                  h->slot_id, h->n_ports, (unsigned)h->pwr_on_2ms * 2);
    return 0;
}

static int hub_configure_slot(xhci_hub_t *h) {
    xhci_controller_t *c = h->ctl;
    uintptr_t inctx_phys;
    uint8_t *inctx = (uint8_t *)dma_alloc_coherent(4096, &inctx_phys);
    if (!inctx) return -ENOMEM;
    memset(inctx, 0, 4096);

    uint32_t *icc = (uint32_t *)inctx;
    icc[0] = 0;
    icc[1] = (1u << 0);

    const uint32_t *out_slot =
        (const uint32_t *)phys_to_virt((uint64_t)c->dcbaa[h->slot_id]);

    uint32_t *slot_ctx = xhci_input_context(c, inctx, 0);
    slot_ctx[0] = out_slot[0];
    slot_ctx[1] = (out_slot[1] & 0x00FFFFFFu) | ((uint32_t)h->n_ports << 24);
    slot_ctx[2] = out_slot[2];
    slot_ctx[3] = out_slot[3] | (1u << 26);

    xhci_trb_t ev;
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0,
                          TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                          ((uint32_t)h->slot_id << 24), &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci-hub] mark-hub slot %u: cc=%u\n", h->slot_id, cc);
        return -EIO;
    }
    serial_printf("[xhci-hub] slot %u marked as Hub (%u ports)\n",
                  h->slot_id, h->n_ports);
    return 0;
}

static int hub_init_topology(xhci_hub_t *h) {
    if (h->n_ports == 0) {
        if (hub_query_descriptor(h) < 0) return -EIO;
    }
    hub_configure_slot(h);
    for (uint8_t p = 1; p <= h->n_ports; p++) {
        hub_set_port_feature(h, p, HUB_FEAT_PORT_POWER);
    }
    usb_msleep((uint32_t)h->pwr_on_2ms * 2 + 20);
    return 0;
}

int xhci_hub_register(xhci_controller_t *c, uint8_t slot_id,
                      const xhci_topology_t *topo,
                      xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                      uint16_t *enq, uint8_t *cyc)
{
    if (g_hub_count >= XHCI_MAX_HUBS) return -ENOMEM;
    xhci_hub_t *h = &g_hubs[g_hub_count++];
    memset(h, 0, sizeof(*h));
    h->ctl       = c;
    h->slot_id   = slot_id;
    h->topo      = *topo;
    h->ep0_ring  = ep0_ring;
    h->ep0_phys  = ep0_phys;
    h->ep0_enq   = *enq;
    h->ep0_cyc   = *cyc;
    h->active    = true;
    serial_printf("[xhci]   HUB skeleton: slot=%u speed=%u root_port=%u route=0x%x\n",
                  slot_id, topo->speed, topo->root_hub_port, topo->route_string);
    return 0;
}

void xhci_hub_post_init(void) {
    for (int i = 0; i < g_hub_count; i++) {
        xhci_hub_t *h = &g_hubs[i];
        if (!h->active) continue;
        if (hub_init_topology(h) < 0) continue;
        for (uint8_t p = 1; p <= h->n_ports; p++) {
            hub_reset_enumerate_port(h, p);
        }
    }
}

static void handle_hub_port(xhci_hub_t *h, uint8_t p) {
    uint32_t st = 0;
    if (hub_get_port_status(h, p, &st) < 0) return;

    if (st & HUB_PC_CONNECTION)
        hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_CONNECTION);

    bool connected = (st & HUB_PS_CONNECTION) != 0;
    int child_idx = xhci_hcd_find_hub_child(h->ctl, h->topo.route_string,
                                            h->topo.depth, p,
                                            h->topo.root_hub_port);

    if (connected && child_idx < 0) {
        serial_printf("[xhci-hp] hub %u port %u: connect\n", h->slot_id, p);
        hub_reset_enumerate_port(h, p);
    } else if (!connected && child_idx >= 0) {
        serial_printf("[xhci-hp] hub %u port %u: disconnect\n", h->slot_id, p);
        xhci_hcd_disconnect_subtree(xhci_hcd_slot_at_index(child_idx));
    }
}

void xhci_hub_tick(void) {
    for (int i = 0; i < g_hub_count; i++) {
        xhci_hub_t *h = &g_hubs[i];
        if (!h->active) continue;
        if (h->n_ports == 0) {
            hub_init_topology(h);
            continue;
        }
        for (uint8_t p = 1; p <= h->n_ports; p++)
            handle_hub_port(h, p);
    }
}

void xhci_hub_disconnect_slot(uint8_t slot_id) {
    for (int i = 0; i < g_hub_count; i++) {
        if (g_hubs[i].active && g_hubs[i].slot_id == slot_id)
            g_hubs[i].active = false;
    }
}
