#include "../usb_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../ehci.h"

#define EHCI_MAX_HUBS 4

#define HUB_FEAT_PORT_RESET            4
#define HUB_FEAT_PORT_POWER            8
#define HUB_FEAT_C_PORT_CONNECTION    16
#define HUB_FEAT_C_PORT_RESET         20

#define HUB_PS_CONNECTION    (1u << 0)
#define HUB_PS_ENABLE        (1u << 1)
#define HUB_PS_LOW_SPEED     (1u << 9)
#define HUB_PS_HIGH_SPEED    (1u << 10)
#define HUB_PC_CONNECTION    (1u << 16)
#define HUB_PC_RESET         (1u << 20)

typedef struct ehci_hub {
    ehci_controller_t *ctl;
    uint8_t  addr;
    uint8_t  n_ports;
    uint8_t  pwr_on_2ms;
    uint8_t  depth;
    uint8_t  parent_addr;
    uint8_t  parent_port;
    uint8_t  root_port;
    uint32_t route_string;
    bool     active;
} ehci_hub_t;

static ehci_hub_t g_hubs[EHCI_MAX_HUBS];
static int g_hub_count = 0;

static int hub_get_descriptor(ehci_hub_t *h, uint8_t *out, uint16_t len) {
    uint8_t setup[8] = { 0xA0, 0x06, 0x00, 0x29, 0x00, 0x00,
                         (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return ehci_control_xfer(h->ctl, h->addr, EHCI_SPEED_HS, 64,
                             setup, out, len, true);
}

static int hub_get_port_status(ehci_hub_t *h, uint8_t port, uint32_t *out) {
    uint8_t buf[4] = {0};
    uint8_t setup[8] = { 0xA3, 0x00, 0x00, 0x00, port, 0x00, 0x04, 0x00 };
    int r = ehci_control_xfer(h->ctl, h->addr, EHCI_SPEED_HS, 64,
                              setup, buf, 4, true);
    if (r < 0) return r;
    *out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return 0;
}

static int hub_set_port_feature(ehci_hub_t *h, uint8_t port, uint16_t feat) {
    uint8_t setup[8] = { 0x23, 0x03,
                         (uint8_t)(feat & 0xFF), (uint8_t)(feat >> 8),
                         port, 0x00, 0x00, 0x00 };
    return ehci_control_xfer(h->ctl, h->addr, EHCI_SPEED_HS, 64,
                             setup, NULL, 0, false);
}

static int hub_clear_port_feature(ehci_hub_t *h, uint8_t port, uint16_t feat) {
    uint8_t setup[8] = { 0x23, 0x01,
                         (uint8_t)(feat & 0xFF), (uint8_t)(feat >> 8),
                         port, 0x00, 0x00, 0x00 };
    return ehci_control_xfer(h->ctl, h->addr, EHCI_SPEED_HS, 64,
                             setup, NULL, 0, false);
}

static void hub_port_enumerate(ehci_hub_t *h, uint8_t p) {
    uint32_t st = 0;
    if (hub_get_port_status(h, p, &st) < 0) return;
    if (!(st & HUB_PS_CONNECTION)) return;

    hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_CONNECTION);

    if (hub_set_port_feature(h, p, HUB_FEAT_PORT_RESET) < 0) {
        serial_printf("[ehci-hub] hub %u port %u: SET PORT_RESET failed\n",
                      h->addr, p);
        return;
    }

    bool reset_done = false;
    for (int j = 0; j < 200; j++) {
        usb_msleep(5);
        if (hub_get_port_status(h, p, &st) < 0) break;
        if (st & HUB_PC_RESET) { reset_done = true; break; }
    }
    if (!reset_done) {
        serial_printf("[ehci-hub] hub %u port %u: reset timeout\n", h->addr, p);
        return;
    }
    hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_RESET);

    if (hub_get_port_status(h, p, &st) < 0) return;
    if (!(st & HUB_PS_ENABLE)) {
        serial_printf("[ehci-hub] hub %u port %u: not enabled (status=0x%x)\n",
                      h->addr, p, st);
        return;
    }

    uint8_t child_speed;
    if (st & HUB_PS_HIGH_SPEED) child_speed = EHCI_SPEED_HS;
    else if (st & HUB_PS_LOW_SPEED) child_speed = EHCI_SPEED_LS;
    else child_speed = EHCI_SPEED_FS;

    if (child_speed != EHCI_SPEED_HS) {
        serial_printf("[ehci-hub] hub %u port %u: LS/FS device (speed=%u) — "
                      "split transactions not yet supported, skipping\n",
                      h->addr, p, child_speed);
        return;
    }

    uint8_t new_addr = ehci_alloc_addr();
    uint8_t setup_addr[8] = { 0x00, 0x05, new_addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    int r = ehci_control_xfer(h->ctl, 0, child_speed, 64,
                              setup_addr, NULL, 0, false);
    if (r < 0) {
        serial_printf("[ehci-hub] hub %u port %u: SET_ADDRESS failed (%d)\n",
                      h->addr, p, r);
        return;
    }
    usb_msleep(2);

    uint32_t shift = (uint32_t)h->depth * 4;
    uint32_t child_route = h->route_string | ((uint32_t)(p & 0xF) << shift);

    char label[32];
    snprintf(label, sizeof(label), "hub%u port %u", h->addr, p);
    ehci_finalize_device(h->ctl, new_addr, child_speed, label,
                         h->root_port, h->addr, p,
                         (uint8_t)(h->depth + 1), child_route);
}

int ehci_hub_setup(ehci_controller_t *c, uint8_t addr,
                   uint8_t root_port, uint8_t parent_addr,
                   uint8_t parent_port, uint8_t depth,
                   uint32_t route_string)
{
    if (g_hub_count >= EHCI_MAX_HUBS) {
        serial_writestring("[ehci-hub] too many hubs, skipping\n");
        return -ENOMEM;
    }
    ehci_hub_t *h = &g_hubs[g_hub_count];
    memset(h, 0, sizeof(*h));
    h->ctl          = c;
    h->addr         = addr;
    h->depth        = depth;
    h->parent_addr  = parent_addr;
    h->parent_port  = parent_port;
    h->root_port    = root_port;
    h->route_string = route_string;

    uint8_t hd[16] = {0};
    if (hub_get_descriptor(h, hd, 8) < 0) {
        serial_printf("[ehci-hub] addr %u: GET_HUB_DESCRIPTOR failed\n", addr);
        return -EIO;
    }
    h->n_ports     = hd[2];
    h->pwr_on_2ms  = hd[5];
    serial_printf("[ehci-hub] addr %u: %u ports, pwrOn2pwrGood=%u ms, hub_chars=0x%04x\n",
                  addr, h->n_ports, (unsigned)h->pwr_on_2ms * 2,
                  (unsigned)((uint16_t)hd[3] | ((uint16_t)hd[4] << 8)));

    for (uint8_t p = 1; p <= h->n_ports; p++) {
        hub_set_port_feature(h, p, HUB_FEAT_PORT_POWER);
    }
    usb_msleep((uint32_t)h->pwr_on_2ms * 2 + 20);

    h->active = true;
    g_hub_count++;

    for (uint8_t p = 1; p <= h->n_ports; p++) {
        hub_port_enumerate(h, p);
    }
    return 0;
}

void ehci_hub_deactivate_addr(uint8_t addr) {
    for (int j = 0; j < g_hub_count; j++) {
        if (g_hubs[j].active && g_hubs[j].addr == addr) {
            g_hubs[j].active = false;
            serial_printf("[ehci-hub] addr %u removed\n", addr);
        }
    }
}
