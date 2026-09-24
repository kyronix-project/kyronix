#include "../usb_glue.h"
#include "../usb_config.h"
#include "../usb_enum.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../ehci.h"

#define EHCI_MAX_DEVS 16
static xhci_pub_dev_t g_ehci_pub_devs[EHCI_MAX_DEVS];
static int g_ehci_pub_devs_count = 0;

int ehci_list_devs(xhci_pub_dev_t *out, int max, uint8_t bus_offset) {
    int n = 0;
    for (int i = 0; i < g_ehci_pub_devs_count && n < max; i++) {
        if (!g_ehci_pub_devs[i].present) continue;
        out[n] = g_ehci_pub_devs[i];
        out[n].ctrl_idx = (uint8_t)(bus_offset + out[n].ctrl_idx);
        n++;
    }
    return n;
}

static ehci_controller_t g_ctrls[EHCI_MAX_CONTROLLERS];
static int g_ctrl_count = 0;

static inline uint32_t cap_r32(ehci_controller_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->bar0 + off);
}
static inline uint8_t cap_r8(ehci_controller_t *c, uint32_t off) {
    uint32_t dw = cap_r32(c, off & ~3u);
    return (uint8_t)((dw >> ((off & 3u) * 8)) & 0xFFu);
}
static inline uint16_t cap_r16(ehci_controller_t *c, uint32_t off) {
    uint32_t dw = cap_r32(c, off & ~3u);
    return (uint16_t)((dw >> ((off & 3u) * 8)) & 0xFFFFu);
}
uint32_t op_r32(ehci_controller_t *c, uint32_t off) {
    return *(volatile uint32_t *)(c->op_regs + off);
}
void op_w32(ehci_controller_t *c, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(c->op_regs + off) = v;
}

static int ehci_bios_handoff(ehci_controller_t *c) {
    pci_dev_t *pd = (pci_dev_t *)c->pdev;
    if (c->eecp < 0x40) {
        c->os_owned = true;
        return 0;
    }
    uint8_t off = c->eecp;
    while (off >= 0x40) {
        uint32_t legsup = pci_read_dword(pd, off);
        uint8_t cap_id = legsup & 0xFF;
        uint8_t next   = (legsup >> 8) & 0xFF;
        if (cap_id == EECP_LEGSUP_CAPID) {
            c->bios_owned_before = (legsup & EECP_LEGSUP_BIOS_OWN) != 0;
            legsup |= EECP_LEGSUP_OS_OWN;
            pci_write_dword(pd, off, legsup);

            for (int i = 0; i < 500; i++) {
                legsup = pci_read_dword(pd, off);
                if (!(legsup & EECP_LEGSUP_BIOS_OWN)) break;
                usb_msleep(2);
            }
            if (legsup & EECP_LEGSUP_BIOS_OWN) {
                serial_writestring("[ehci] BIOS won't release ownership; forcing OS-only\n");
                legsup &= ~EECP_LEGSUP_BIOS_OWN;
                pci_write_dword(pd, off, legsup);
            }
            pci_write_dword(pd, off + 4, 0);

            c->os_owned = true;
            serial_printf("[ehci] BIOS handoff complete (was BIOS-owned=%d)\n",
                          c->bios_owned_before);
            return 0;
        }
        if (next == 0) break;
        off = next;
    }
    c->os_owned = true;
    return 0;
}

static int ehci_halt(ehci_controller_t *c) {
    uint32_t cmd = op_r32(c, EHCI_OP_USBCMD);
    op_w32(c, EHCI_OP_USBCMD, cmd & ~(USBCMD_RUN | USBCMD_ASE | USBCMD_PSE));
    for (int i = 0; i < 100; i++) {
        if (op_r32(c, EHCI_OP_USBSTS) & USBSTS_HCHALTED) return 0;
        usb_msleep(1);
    }
    return -EIO;
}

static int ehci_reset(ehci_controller_t *c) {
    uint32_t cmd = op_r32(c, EHCI_OP_USBCMD);
    op_w32(c, EHCI_OP_USBCMD, cmd | USBCMD_HCRESET);
    for (int i = 0; i < 500; i++) {
        if ((op_r32(c, EHCI_OP_USBCMD) & USBCMD_HCRESET) == 0) return 0;
        usb_msleep(1);
    }
    return -EIO;
}

static int ehci_alloc_schedules(ehci_controller_t *c) {
    uintptr_t phys;
    void *page = dma_alloc_coherent_low(4096, &phys);
    if (!page) return -ENOMEM;
    if (phys >= 0xFFFFFFFFULL) {
        serial_writestring("[ehci] periodic list >4GB, abort\n");
        return -EIO;
    }
    c->periodic_list = (uint32_t *)page;
    c->periodic_phys = phys;
    for (int i = 0; i < 1024; i++) c->periodic_list[i] = QH_T;

    uintptr_t qh_phys;
    ehci_qh_t *head = (ehci_qh_t *)dma_alloc_coherent_low(4096, &qh_phys);
    if (!head) return -ENOMEM;
    if (qh_phys >= 0xFFFFFFFFULL) {
        serial_writestring("[ehci] async head >4GB, abort\n");
        return -EIO;
    }
    memset(head, 0, sizeof(*head));
    head->hlp           = ((uint32_t)qh_phys) | QH_TYPE_QH;
    head->ep_chars      = (1u << 15);
    head->ep_caps       = 0;
    head->cur_qtd       = 0;
    head->overlay_next  = QTD_T;
    head->overlay_alt_next = QTD_T;
    head->overlay_token = QTD_STATUS_HALTED;

    c->async_head      = head;
    c->async_head_phys = qh_phys;
    return 0;
}

static int ehci_start(ehci_controller_t *c) {
    op_w32(c, EHCI_OP_CTRLDSSEG,    0);
    op_w32(c, EHCI_OP_PERIODICLIST, (uint32_t)c->periodic_phys);
    op_w32(c, EHCI_OP_ASYNCLIST,    (uint32_t)c->async_head_phys);
    op_w32(c, EHCI_OP_USBINTR,      0);
    op_w32(c, EHCI_OP_FRINDEX,      0);

    uint32_t cmd = (8u << 16) | USBCMD_RUN | USBCMD_ASE;
    op_w32(c, EHCI_OP_USBCMD, cmd);

    for (int i = 0; i < 200; i++) {
        if ((op_r32(c, EHCI_OP_USBSTS) & USBSTS_HCHALTED) == 0) {
            c->running = true;
            break;
        }
        usb_msleep(1);
    }
    if (!c->running) return -EIO;

    op_w32(c, EHCI_OP_CONFIGFLAG, CONFIGFLAG_CF);

    for (uint8_t p = 0; p < c->n_ports; p++) {
        uint32_t pc = op_r32(c, EHCI_OP_PORTSC(p));
        pc &= ~(PORTSC_OWNER);
        if (c->port_power_control) pc |= PORTSC_PORT_POWER;
        op_w32(c, EHCI_OP_PORTSC(p), pc);
    }
    usb_msleep(50);
    return 0;
}

static int ehci_port_reset(ehci_controller_t *c, uint8_t port) {
    uint32_t pc = op_r32(c, EHCI_OP_PORTSC(port));
    if (!(pc & PORTSC_CONNECT)) return -ENODEV;

    if ((pc & PORTSC_LS_MASK) == PORTSC_LS_KSTATE) {
        serial_printf("[ehci] port %u: low-speed device, releasing to companion\n",
                      port + 1);
        op_w32(c, EHCI_OP_PORTSC(port), pc | PORTSC_OWNER);
        return -EIO;
    }

    uint32_t base = pc & ~(PORTSC_ENABLE | PORTSC_CONNECT_CHG |
                           PORTSC_ENABLE_CHG | PORTSC_OCC_CHG);
    op_w32(c, EHCI_OP_PORTSC(port), base | PORTSC_RESET);
    usb_msleep(55);
    pc = op_r32(c, EHCI_OP_PORTSC(port));
    op_w32(c, EHCI_OP_PORTSC(port), pc & ~PORTSC_RESET);

    for (int i = 0; i < 50; i++) {
        pc = op_r32(c, EHCI_OP_PORTSC(port));
        if (!(pc & PORTSC_RESET)) break;
        usb_msleep(1);
    }
    if (pc & PORTSC_RESET) return -EIO;

    pc = op_r32(c, EHCI_OP_PORTSC(port));
    if (!(pc & PORTSC_ENABLE)) {
        serial_printf("[ehci] port %u: not high-speed, releasing to companion\n",
                      port + 1);
        op_w32(c, EHCI_OP_PORTSC(port), pc | PORTSC_OWNER);
        return -EIO;
    }
    usb_msleep(15);
    return 0;
}

ehci_qtd_t *alloc_qtd(uintptr_t *out_phys) {
    void *p = dma_alloc_coherent_low(4096, out_phys);
    if (!p) return NULL;
    if (*out_phys >= 0xFFFFFFFFULL) { dma_free_coherent(p, 4096); return NULL; }
    memset(p, 0, sizeof(ehci_qtd_t));
    return (ehci_qtd_t *)p;
}

void qtd_set_buf(ehci_qtd_t *t, uintptr_t buf_phys, uint32_t len) {
    memset(t->buf, 0, sizeof(t->buf));
    if (len == 0) return;
    t->buf[0] = (uint32_t)buf_phys;
    uint32_t off = (uint32_t)(buf_phys & 0xFFF);
    uint32_t base = (uint32_t)(buf_phys & ~0xFFFULL);
    uint32_t covered = 0x1000 - off;
    for (int i = 1; i < 5 && covered < len; i++) {
        base += 0x1000;
        t->buf[i] = base;
        covered += 0x1000;
    }
}

int ehci_control_xfer(ehci_controller_t *c,
                             uint8_t addr, uint8_t speed, uint16_t mps,
                             const uint8_t setup[8], void *data, uint16_t data_len,
                             bool data_in)
{
    uintptr_t setup_phys;
    void *setup_buf = dma_alloc_coherent_low(64, &setup_phys);
    if (!setup_buf || setup_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    memcpy(setup_buf, setup, 8);

    uintptr_t data_phys = 0;
    void *data_buf = NULL;
    if (data_len > 0) {
        data_buf = dma_alloc_coherent_low(data_len, &data_phys);
        if (!data_buf || data_phys >= 0xFFFFFFFFULL) {
            if (setup_buf) dma_free_coherent(setup_buf, 64);
            return -ENOMEM;
        }
        if (!data_in) memcpy(data_buf, data, data_len);
        else memset(data_buf, 0, data_len);
    }

    uintptr_t t1p, t2p, t3p;
    ehci_qtd_t *t1 = alloc_qtd(&t1p);
    ehci_qtd_t *t2 = alloc_qtd(&t2p);
    ehci_qtd_t *t3 = alloc_qtd(&t3p);
    uintptr_t qh_phys;
    ehci_qh_t *qh = (ehci_qh_t *)dma_alloc_coherent_low(4096, &qh_phys);
    if (!t1 || !t2 || !t3 || !qh || qh_phys >= 0xFFFFFFFFULL) {
        if (t1) dma_free_coherent(t1, 4096);
        if (t2) dma_free_coherent(t2, 4096);
        if (t3) dma_free_coherent(t3, 4096);
        if (qh) dma_free_coherent(qh, 4096);
        dma_free_coherent(setup_buf, 64);
        if (data_buf) dma_free_coherent(data_buf, data_len);
        return -ENOMEM;
    }
    memset(qh, 0, sizeof(*qh));

    t1->alt_next = QTD_T;
    t1->token    = QTD_STATUS_ACTIVE | QTD_PID_SETUP | QTD_CERR_3 |
                   ((uint32_t)8 << QTD_TBT_SHIFT);
    qtd_set_buf(t1, setup_phys, 8);

    if (data_len > 0) {
        t1->next     = (uint32_t)t2p;
        t2->next     = (uint32_t)t3p;
        t2->alt_next = QTD_T;
        t2->token    = QTD_STATUS_ACTIVE | QTD_CERR_3 | QTD_DT |
                       ((uint32_t)data_len << QTD_TBT_SHIFT) |
                       (data_in ? QTD_PID_IN : QTD_PID_OUT);
        qtd_set_buf(t2, data_phys, data_len);
    } else {
        t1->next     = (uint32_t)t3p;
        t2->token    = 0;
    }

    t3->next     = QTD_T;
    t3->alt_next = QTD_T;
    t3->token    = QTD_STATUS_ACTIVE | QTD_CERR_3 | QTD_DT | QTD_IOC |
                   (data_in ? QTD_PID_OUT : QTD_PID_IN);

    spin_lock(&c->async_lock);

    uint32_t c_flag = (speed == EHCI_SPEED_HS) ? 0u : (1u << 27);
    qh->hlp           = c->async_head->hlp;
    qh->ep_chars      = ((uint32_t)addr & 0x7F)
                      | (0u << 8)
                      | ((uint32_t)speed << 12)
                      | (1u << 14)
                      | ((uint32_t)mps << 16)
                      | c_flag
                      | (5u << 28);
    qh->ep_caps       = (1u << 30);
    qh->cur_qtd       = 0;
    qh->overlay_next  = (uint32_t)t1p;
    qh->overlay_alt_next = QTD_T;
    qh->overlay_token = 0;

    __asm__ volatile("" ::: "memory");

    c->async_head->hlp = (uint32_t)qh_phys | QH_TYPE_QH;

    __asm__ volatile("" ::: "memory");

    uint64_t deadline = usb_now_ns() + 2000000000ULL;
    int r = -EIO;
    for (;;) {
        uint32_t st = t3->token & 0xFF;
        if (!(st & QTD_STATUS_ACTIVE)) {
            if (st & (QTD_STATUS_HALTED | QTD_STATUS_DBE | QTD_STATUS_BABBLE |
                      QTD_STATUS_XACTERR | QTD_STATUS_MISSED)) {
                serial_printf("[ehci] xfer error: t3_status=0x%02x t1_tok=0x%x\n",
                              st, t1->token);
                r = -EIO;
            } else {
                r = 0;
            }
            break;
        }
        if (usb_now_ns() > deadline) break;
        __asm__ volatile("pause");
    }

    c->async_head->hlp = qh->hlp;

    uint32_t cmd = op_r32(c, EHCI_OP_USBCMD);
    op_w32(c, EHCI_OP_USBCMD, cmd | USBCMD_IAAD);
    uint64_t dl2 = usb_now_ns() + 100000000ULL;
    while (!(op_r32(c, EHCI_OP_USBSTS) & USBSTS_IAA)) {
        if (usb_now_ns() > dl2) break;
        __asm__ volatile("pause");
    }
    op_w32(c, EHCI_OP_USBSTS, USBSTS_IAA);

    spin_unlock(&c->async_lock);

    if (r == 0 && data_in && data_len > 0) memcpy(data, data_buf, data_len);

    dma_free_coherent(setup_buf, 64);
    if (data_buf) dma_free_coherent(data_buf, data_len);
    dma_free_coherent(t1, 4096);
    dma_free_coherent(t2, 4096);
    dma_free_coherent(t3, 4096);
    dma_free_coherent(qh, 4096);
    return r;
}

int ehci_async_pause(ehci_controller_t *c, uint32_t *old_cmd) {
    spin_lock(&c->async_lock);
    *old_cmd = op_r32(c, EHCI_OP_USBCMD);
    op_w32(c, EHCI_OP_USBCMD, *old_cmd & ~USBCMD_ASE);
    uint64_t deadline = usb_now_ns() + 100000000ULL;
    while (op_r32(c, EHCI_OP_USBSTS) & USBSTS_ASYNC) {
        if (usb_now_ns() > deadline) {
            spin_unlock(&c->async_lock);
            return -EIO;
        }
        __asm__ volatile("pause");
    }
    return 0;
}

void ehci_async_resume(ehci_controller_t *c, uint32_t old_cmd) {
    op_w32(c, EHCI_OP_USBCMD, old_cmd | USBCMD_ASE);
    uint64_t deadline = usb_now_ns() + 100000000ULL;
    while (!(op_r32(c, EHCI_OP_USBSTS) & USBSTS_ASYNC)) {
        if (usb_now_ns() > deadline) break;
        __asm__ volatile("pause");
    }
    spin_unlock(&c->async_lock);
}

static void ehci_handle_root_port(ehci_controller_t *c, uint8_t port);

void ehci_worker(void) {
    serial_writestring("[ehci-worker] started\n");
    uint32_t slow = 0;
    while (1) {
        ehci_hid_kbd_tick();

        if ((slow++ % 31) == 0) {
            int nc = ehci_controller_count();
            for (int i = 0; i < nc; i++) {
                ehci_controller_t *c = ehci_get_controller(i);
                if (!c || !c->ready) continue;
                for (uint8_t p = 0; p < c->n_ports; p++)
                    ehci_handle_root_port(c, p);
            }
        }
        usb_msleep(16);
    }
}

static uint8_t g_next_addr = 1;

uint8_t ehci_alloc_addr(void) {
    return g_next_addr++;
}

static const char *usb_class_name(uint8_t c) {
    switch (c) {
        case 0x00: return "(per-interface)";
        case 0x01: return "Audio";
        case 0x02: return "CDC";
        case 0x03: return "HID";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC Data";
        case 0x0B: return "Smart Card";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFF: return "Vendor";
        default:   return "?";
    }
}

typedef struct {
    ehci_controller_t *ctl;
    uint8_t  addr;
    uint8_t  speed;
    uint16_t mps;
} ehci_ctrl_handle_t;

static int ehci_enum_control(void *h, const uint8_t setup[8],
                             void *data, uint16_t len, bool in) {
    ehci_ctrl_handle_t *eh = (ehci_ctrl_handle_t *)h;
    return ehci_control_xfer(eh->ctl, eh->addr, eh->speed, eh->mps,
                             setup, data, len, in);
}

void ehci_finalize_device(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                                 const char *label,
                                 uint8_t root_port, uint8_t parent_addr,
                                 uint8_t parent_port, uint8_t depth,
                                 uint32_t route_string)
{
    ehci_ctrl_handle_t eh = { c, addr, speed, 64 };
    usb_ctrl_ops_t ops = { &eh, ehci_enum_control };

    uint8_t desc[18] = {0};
    int r = usb_get_device_descriptor(&ops, desc);
    if (r < 0) {
        serial_printf("[ehci] %s addr %u: GET_DESCRIPTOR(DEVICE) failed (%d)\n",
                      label, addr, r);
        return;
    }
    uint16_t vid = (uint16_t)desc[8]  | ((uint16_t)desc[9]  << 8);
    uint16_t pid = (uint16_t)desc[10] | ((uint16_t)desc[11] << 8);
    uint16_t bcd = (uint16_t)desc[2]  | ((uint16_t)desc[3]  << 8);
    uint16_t bcd_dev = (uint16_t)desc[12] | ((uint16_t)desc[13] << 8);
    uint8_t  ep0_mps = desc[7] ? desc[7] : 64;
    uint8_t  n_cfg = desc[17];
    serial_printf("[ehci] %s addr %u: USB %u.%u  VID=0x%04x PID=0x%04x  "
                  "class=0x%02x (%s)  EP0_MPS=%u  nConfigs=%u\n",
                  label, addr,
                  (bcd >> 8) & 0xFF, (bcd >> 4) & 0xF,
                  vid, pid, desc[4], usb_class_name(desc[4]), ep0_mps, n_cfg);

    xhci_pub_dev_t *pd = NULL;
    for (int i = 0; i < g_ehci_pub_devs_count; i++) {
        if (!g_ehci_pub_devs[i].present) { pd = &g_ehci_pub_devs[i]; break; }
    }
    if (!pd && g_ehci_pub_devs_count < EHCI_MAX_DEVS)
        pd = &g_ehci_pub_devs[g_ehci_pub_devs_count++];
    if (pd) {
        memset(pd, 0, sizeof(*pd));
        pd->ctrl_idx   = 0;
        pd->slot_id    = addr;
        pd->port_id    = root_port;
        pd->speed      = speed == EHCI_SPEED_HS ? 3
                       : speed == EHCI_SPEED_FS ? 1 : 2;
        pd->vid        = vid;
        pd->pid        = pid;
        pd->bcd_dev    = bcd_dev;
        pd->bcd_usb    = bcd;
        pd->dev_class  = desc[4];
        pd->dev_sub    = desc[5];
        pd->dev_proto  = desc[6];
        pd->ep0_mps    = ep0_mps;
        pd->n_configs  = n_cfg;
        pd->parent_slot = parent_addr;
        pd->parent_port = parent_port;
        pd->depth      = depth;
        pd->route_string = route_string;
        pd->present    = 1;
    }

    if (n_cfg == 0) return;

    eh.mps = ep0_mps;

    uint8_t cfg_full[1024];
    uint16_t total = 0;
    uint8_t cfg_value = 0;
    r = usb_get_config(&ops, cfg_full, sizeof(cfg_full), &total, &cfg_value);
    if (r < 0) {
        serial_printf("[ehci]   GET_DESCRIPTOR(config): failed (%d)\n", r);
        return;
    }
    usb_ifaces_t ifaces;
    usb_parse_config(cfg_full, total, &ifaces);

    ehci_msc_info_t msc_info;
    ehci_hid_info_t hid_info;
    memset(&msc_info, 0, sizeof(msc_info));
    memset(&hid_info, 0, sizeof(hid_info));
    if (ifaces.msc.present) {
        msc_info.is_msc  = true;
        msc_info.intf    = ifaces.msc.intf;
        msc_info.in_ep   = ifaces.msc.in_ep;
        msc_info.in_mps  = ifaces.msc.in_mps;
        msc_info.out_ep  = ifaces.msc.out_ep;
        msc_info.out_mps = ifaces.msc.out_mps;
    }
    if (ifaces.hid.present) {
        hid_info.is_kbd   = !ifaces.hid.is_mouse;
        hid_info.is_mouse = ifaces.hid.is_mouse;
        hid_info.intf     = ifaces.hid.intf;
        hid_info.in_ep    = ifaces.hid.in_ep;
        hid_info.in_mps   = ifaces.hid.in_mps;
        hid_info.interval = ifaces.hid.interval;
    }

    if (pd) {
        pd->intf_class = ifaces.first_class;
        pd->intf_sub   = ifaces.first_sub;
        pd->intf_proto = ifaces.first_proto;
    }

    r = usb_set_configuration(&ops, cfg_value);
    if (r < 0) {
        serial_printf("[ehci]   SET_CONFIGURATION %u: failed (%d)\n", cfg_value, r);
        return;
    }
    serial_printf("[ehci]   SET_CONFIGURATION %u: ok\n", cfg_value);

    if (msc_info.is_msc && msc_info.in_ep && msc_info.out_ep) {
        uint8_t setup_maxlun[8] = { 0xA1, 0xFE, 0x00, 0x00,
                                    msc_info.intf, 0x00, 0x01, 0x00 };
        uint8_t maxlun = 0;
        (void)ehci_control_xfer(c, addr, speed, ep0_mps,
                                setup_maxlun, &maxlun, 1, true);
        serial_printf("[ehci]   MSC: intf=%u IN=ep%u (mps=%u) OUT=ep%u (mps=%u) maxLUN=%u\n",
                      msc_info.intf, msc_info.in_ep, msc_info.in_mps,
                      msc_info.out_ep, msc_info.out_mps, maxlun);
        if (ehci_msc_setup(c, addr, speed, &msc_info) < 0)
            serial_writestring("[ehci]   MSC setup failed\n");
    } else if ((hid_info.is_kbd || hid_info.is_mouse) && hid_info.in_ep) {
        serial_printf("[ehci]   HID %s: intf=%u IN=ep%u (mps=%u, interval=%u)\n",
                      hid_info.is_mouse ? "mouse" : "kbd",
                      hid_info.intf, hid_info.in_ep, hid_info.in_mps,
                      hid_info.interval);
        if (ehci_hid_kbd_setup(c, addr, speed, &hid_info) < 0)
            serial_writestring("[ehci]   HID setup failed\n");
    } else if (desc[4] == 0x09 || (pd && pd->intf_class == 0x09)) {
        if (speed == EHCI_SPEED_HS) {
            if (ehci_hub_setup(c, addr, root_port, parent_addr,
                               parent_port, depth, route_string) < 0)
                serial_writestring("[ehci]   Hub setup failed\n");
        } else {
            serial_writestring("[ehci]   Hub: only HS hubs supported; skipping\n");
        }
    }
}

static void ehci_enumerate_port(ehci_controller_t *c, uint8_t port) {
    if (ehci_port_reset(c, port) < 0) return;

    uint8_t addr = g_next_addr++;
    uint8_t setup_addr[8] = { 0x00, 0x05, addr, 0x00, 0x00, 0x00, 0x00, 0x00 };
    int r = ehci_control_xfer(c, 0, EHCI_SPEED_HS, 64, setup_addr, NULL, 0, false);
    if (r < 0) {
        serial_printf("[ehci] port %u: SET_ADDRESS failed (%d)\n", port + 1, r);
        return;
    }
    usb_msleep(2);

    char label[16];
    snprintf(label, sizeof(label), "port %u", port + 1);
    ehci_finalize_device(c, addr, EHCI_SPEED_HS, label,
                         (uint8_t)(port + 1), 0, 0, 0, 0);
}

static void ehci_enumerate(ehci_controller_t *c) {
    int found = 0;
    for (uint8_t p = 0; p < c->n_ports; p++) {
        uint32_t pc = op_r32(c, EHCI_OP_PORTSC(p));
        if (!(pc & PORTSC_CONNECT)) continue;
        if (pc & PORTSC_OWNER) continue;
        ehci_enumerate_port(c, p);
        found++;
    }
    if (!found) serial_writestring("[ehci] no high-speed devices enumerated\n");
}

static int ehci_find_root_dev(uint8_t port) {
    for (int i = 0; i < g_ehci_pub_devs_count; i++) {
        if (g_ehci_pub_devs[i].present &&
            g_ehci_pub_devs[i].port_id == port &&
            g_ehci_pub_devs[i].depth == 0)
            return i;
    }
    return -1;
}

static void ehci_teardown_addr(uint8_t addr) {
    ehci_msc_deactivate_addr(addr);
    ehci_hid_kbd_deactivate_addr(addr);
    ehci_hub_deactivate_addr(addr);
}

static void ehci_disconnect_port(uint8_t port) {
    for (int i = 0; i < g_ehci_pub_devs_count; i++) {
        xhci_pub_dev_t *u = &g_ehci_pub_devs[i];
        if (!u->present) continue;
        if (u->port_id != port) continue;
        uint8_t addr = u->slot_id;
        u->present = 0;
        serial_printf("[ehci-hp] addr %u disconnected (vid:pid=%04x:%04x)\n",
                      addr, u->vid, u->pid);
        ehci_teardown_addr(addr);
    }
}

static uint8_t  g_ehci_port_state[EHCI_MAX_CONTROLLERS][16];
static uint32_t g_ehci_last_portsc[EHCI_MAX_CONTROLLERS][16];

static void ehci_handle_root_port(ehci_controller_t *c, uint8_t port) {
    int cidx = (int)(c - g_ctrls);
    if (cidx < 0 || cidx >= EHCI_MAX_CONTROLLERS || port >= 16) return;

    uint32_t pc = op_r32(c, EHCI_OP_PORTSC(port));

    uint32_t stable_mask = PORTSC_CONNECT | PORTSC_ENABLE | PORTSC_OWNER |
                           PORTSC_SUSPEND | PORTSC_RESET | PORTSC_LS_MASK;
    if ((pc & stable_mask) != (g_ehci_last_portsc[cidx][port] & stable_mask)) {
        serial_printf("[ehci-hp] ctrl%d port %u PORTSC 0x%08x -> 0x%08x\n",
                      cidx, port + 1, g_ehci_last_portsc[cidx][port], pc);
    }
    g_ehci_last_portsc[cidx][port] = pc;

    uint32_t chg = pc & (PORTSC_CONNECT_CHG | PORTSC_ENABLE_CHG | PORTSC_OCC_CHG);
    if (chg) {
        uint32_t v = pc & (PORTSC_PORT_POWER | PORTSC_OWNER | PORTSC_ENABLE);
        v |= chg;
        op_w32(c, EHCI_OP_PORTSC(port), v);
    }

    bool connected = (pc & PORTSC_CONNECT) != 0;
    bool was = g_ehci_port_state[cidx][port] != 0;
    if (connected == was) return;
    g_ehci_port_state[cidx][port] = connected ? 1 : 0;

    int existing = ehci_find_root_dev((uint8_t)(port + 1));

    if (connected && existing < 0) {
        if (pc & PORTSC_OWNER) {
            serial_printf("[ehci-hp] port %u: connect but OWNER=companion (UHCI)\n", port + 1);
            return;
        }
        serial_printf("[ehci-hp] port %u: connect, enumerating...\n", port + 1);
        ehci_enumerate_port(c, port);
    } else if (!connected && existing >= 0) {
        serial_printf("[ehci-hp] port %u: disconnect\n", port + 1);
        ehci_disconnect_port((uint8_t)(port + 1));
    }
}

static void ehci_probe(pci_dev_t *pd) {
    if (g_ctrl_count >= EHCI_MAX_CONTROLLERS) return;
    if (pd->class != 0x0C || pd->subclass != 0x03 || pd->prog_if != 0x20) return;

    uintptr_t bar_base = (uintptr_t)pd->bars[0];
    if (bar_base == 0) return;

    uint64_t bar_size = pd->bar_sizes[0] ? pd->bar_sizes[0] : 0x1000;
    volatile uint8_t *bar_virt =
        (volatile uint8_t *)mmio_map(bar_base, (size_t)bar_size);
    if (!bar_virt) return;

    uint32_t cmd_reg = pci_read32(pd->bus, pd->dev, pd->fn, 0x04);
    cmd_reg |= 0x2 | 0x4 | 0x400;
    pci_write32(pd->bus, pd->dev, pd->fn, 0x04, cmd_reg);

    ehci_controller_t *c = &g_ctrls[g_ctrl_count];
    memset(c, 0, sizeof(*c));
    c->pdev     = pd;
    c->bar0     = bar_virt;
    c->bar_size = (uint32_t)bar_size;

    c->caplength  = cap_r8(c, EHCI_CAP_CAPLENGTH);
    c->hciversion = cap_r16(c, EHCI_CAP_HCIVERSION);
    c->hcsparams  = cap_r32(c, EHCI_CAP_HCSPARAMS);
    c->hccparams  = cap_r32(c, EHCI_CAP_HCCPARAMS);
    c->n_ports    = (uint8_t)(c->hcsparams & 0xF);
    c->port_power_control = (c->hcsparams & (1u << 4)) != 0;
    c->has_64bit  = (c->hccparams & (1u << 0)) != 0;
    c->eecp       = (uint8_t)((c->hccparams >> 8) & 0xFF);
    c->op_regs    = bar_virt + c->caplength;

    serial_printf("[ehci] BAR0=0x%llx caplen=%u HCIVERSION=%u.%u ports=%u "
                  "EECP=0x%02x PPC=%d 64bit=%d\n",
                  (unsigned long long)bar_base, c->caplength,
                  (c->hciversion >> 8) & 0xFF, c->hciversion & 0xFF,
                  c->n_ports, c->eecp, c->port_power_control, c->has_64bit);

    if (ehci_bios_handoff(c) < 0) {
        serial_writestring("[ehci] BIOS handoff failed\n");
        return;
    }

    if (ehci_halt(c) < 0) serial_writestring("[ehci] halt timeout (continuing)\n");
    if (ehci_reset(c) < 0) {
        serial_writestring("[ehci] reset timeout\n");
        return;
    }

    if (ehci_alloc_schedules(c) < 0) return;
    if (ehci_start(c) < 0) {
        serial_writestring("[ehci] start timeout\n");
        return;
    }
    serial_writestring("[ehci] controller running, enumerating high-speed devices\n");

    ehci_enumerate(c);

    if (g_ctrl_count < EHCI_MAX_CONTROLLERS) {
        for (uint8_t p = 0; p < c->n_ports && p < 16; p++) {
            uint32_t pc = op_r32(c, EHCI_OP_PORTSC(p));
            g_ehci_port_state[g_ctrl_count][p] = (pc & PORTSC_CONNECT) ? 1 : 0;
        }
    }

    c->ready = true;
    g_ctrl_count++;
}

void ehci_init(void) {
    usb_pci_for_each(0x0C, 0x03, 0x20, ehci_probe);
}

void ehci_start_worker(void) {
    static bool spawned = false;
    if (spawned) return;
    if (g_ctrl_count == 0) return;
    spawned = true;
    extern void ehci_worker(void);
    proc_create_kernel("ehci_worker", ehci_worker);
}

int ehci_controller_count(void) { return g_ctrl_count; }
ehci_controller_t *ehci_get_controller(int idx) {
    if (idx < 0 || idx >= g_ctrl_count) return NULL;
    return &g_ctrls[idx];
}

