#include "../usb_glue.h"
#include "../lib/string.h"
#include "../uhci.h"

#define UHCI_MAX_HUBS 4

#define HUB_FEAT_PORT_RESET         4
#define HUB_FEAT_PORT_POWER         8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_RESET      20

#define HUB_PS_CONNECTION  (1u << 0)
#define HUB_PS_ENABLE      (1u << 1)
#define HUB_PS_LOW_SPEED   (1u << 9)
#define HUB_PC_CONNECTION  (1u << 16)
#define HUB_PC_RESET       (1u << 20)

typedef struct {
    uhci_controller_t *ctl;
    uint8_t  addr;
    uint8_t  low_speed;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  n_ports;
    uint8_t  pwr_on_2ms;
    uint8_t  last_conn[16];
    bool     active;
} uhci_hub_t;

static uhci_hub_t g_hubs[UHCI_MAX_HUBS];
static int        g_hub_count = 0;

static int hub_get_descriptor(uhci_hub_t *h, uint8_t *out, uint16_t len) {
    uint8_t setup[8] = { 0xA0, 0x06, 0x00, 0x29, 0x00, 0x00,
                         (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    return uhci_control_xfer(h->ctl, h->addr, h->low_speed,
                             h->low_speed ? 8 : 64, setup, out, len, true);
}

static int hub_get_port_status(uhci_hub_t *h, uint8_t port, uint32_t *out) {
    uint8_t buf[4] = {0};
    uint8_t setup[8] = { 0xA3, 0x00, 0x00, 0x00, port, 0x00, 0x04, 0x00 };
    int r = uhci_control_xfer(h->ctl, h->addr, h->low_speed,
                              h->low_speed ? 8 : 64, setup, buf, 4, true);
    if (r < 0) return r;
    *out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return 0;
}

static int hub_set_port_feature(uhci_hub_t *h, uint8_t port, uint16_t feat) {
    uint8_t setup[8] = { 0x23, 0x03,
                         (uint8_t)(feat & 0xFF), (uint8_t)(feat >> 8),
                         port, 0x00, 0x00, 0x00 };
    return uhci_control_xfer(h->ctl, h->addr, h->low_speed,
                             h->low_speed ? 8 : 64, setup, NULL, 0, false);
}

static int hub_clear_port_feature(uhci_hub_t *h, uint8_t port, uint16_t feat) {
    uint8_t setup[8] = { 0x23, 0x01,
                         (uint8_t)(feat & 0xFF), (uint8_t)(feat >> 8),
                         port, 0x00, 0x00, 0x00 };
    return uhci_control_xfer(h->ctl, h->addr, h->low_speed,
                             h->low_speed ? 8 : 64, setup, NULL, 0, false);
}

static void hub_port_enumerate(uhci_hub_t *h, uint8_t p) {
    uint32_t st = 0;
    if (hub_get_port_status(h, p, &st) < 0) return;
    if (!(st & HUB_PS_CONNECTION)) return;

    hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_CONNECTION);

    if (hub_set_port_feature(h, p, HUB_FEAT_PORT_RESET) < 0) {
        serial_printf("[uhci-hub] hub %u port %u: set RESET failed\n", h->addr, p);
        return;
    }
    bool reset_done = false;
    for (int j = 0; j < 100; j++) {
        usb_msleep(10);
        if (hub_get_port_status(h, p, &st) < 0) break;
        if (st & HUB_PC_RESET) { reset_done = true; break; }
    }
    if (!reset_done) {
        serial_printf("[uhci-hub] hub %u port %u: reset timeout\n", h->addr, p);
        return;
    }
    hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_RESET);

    serial_printf("[uhci-hub] hub %u port %u: downstream device connected (status=0x%08x)\n",
                  h->addr, p, st);
}

int uhci_hub_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                   const uhci_hub_info_t *info)
{
    uhci_hub_t *h = NULL;
    for (int i = 0; i < g_hub_count; i++) {
        if (!g_hubs[i].active) { h = &g_hubs[i]; break; }
    }
    if (!h) {
        if (g_hub_count >= UHCI_MAX_HUBS) return -ENOMEM;
        h = &g_hubs[g_hub_count++];
    }
    memset(h, 0, sizeof(*h));
    h->ctl       = c;
    h->addr      = addr;
    h->low_speed = low_speed ? 1 : 0;
    h->intf      = info->intf;
    h->in_ep     = info->in_ep;
    h->in_mps    = info->in_mps;

    uint8_t hd[8] = {0};
    if (hub_get_descriptor(h, hd, sizeof(hd)) < 0) {
        serial_printf("[uhci-hub] addr %u: GET_HUB_DESCRIPTOR failed\n", addr);
        return -EIO;
    }
    h->n_ports = hd[2];
    h->pwr_on_2ms = hd[5];
    if (h->n_ports > 16) h->n_ports = 16;

    serial_printf("[uhci-hub] addr %u: %u downstream port(s) pwrOn=%u(*2ms)\n",
                  addr, h->n_ports, h->pwr_on_2ms);

    for (uint8_t p = 1; p <= h->n_ports; p++) {
        hub_set_port_feature(h, p, HUB_FEAT_PORT_POWER);
    }
    usb_msleep(h->pwr_on_2ms ? (h->pwr_on_2ms * 2 + 20) : 100);

    h->active = true;

    for (uint8_t p = 1; p <= h->n_ports; p++) {
        hub_port_enumerate(h, p);
    }
    return 0;
}

void uhci_hub_tick(void) {
    for (int i = 0; i < g_hub_count; i++) {
        uhci_hub_t *h = &g_hubs[i];
        if (!h->active) continue;
        for (uint8_t p = 1; p <= h->n_ports; p++) {
            uint32_t st = 0;
            if (hub_get_port_status(h, p, &st) < 0) continue;
            uint8_t conn = (st & HUB_PS_CONNECTION) ? 1 : 0;
            uint8_t prev = (p - 1 < 16) ? h->last_conn[p - 1] : 0;
            if (st & HUB_PC_CONNECTION) {
                hub_clear_port_feature(h, p, HUB_FEAT_C_PORT_CONNECTION);
            }
            if (conn != prev) {
                if (p - 1 < 16) h->last_conn[p - 1] = conn;
                if (conn) {
                    serial_printf("[uhci-hub] hub %u port %u: connect\n", h->addr, p);
                    hub_port_enumerate(h, p);
                } else {
                    serial_printf("[uhci-hub] hub %u port %u: disconnect\n", h->addr, p);
                }
            }
        }
    }
}

void uhci_hub_deactivate_addr(uint8_t addr) {
    for (int i = 0; i < g_hub_count; i++) {
        if (g_hubs[i].active && g_hubs[i].addr == addr) {
            g_hubs[i].active = false;
            serial_printf("[uhci-hub] addr %u removed\n", addr);
        }
    }
}
