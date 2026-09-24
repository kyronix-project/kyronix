#ifndef KERNEL_DRIVERS_XHCI_H
#define KERNEL_DRIVERS_XHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define XHCI_MAX_CONTROLLERS 2
#define XHCI_CMD_RING_TRBS   256
#define XHCI_EVT_RING_TRBS   64

typedef struct xhci_controller xhci_controller_t;

typedef struct __attribute__((packed)) {
    uint8_t  ctrl_idx;
    uint8_t  slot_id;
    uint8_t  port_id;
    uint8_t  speed;
    uint16_t vid;
    uint16_t pid;
    uint16_t bcd_dev;
    uint16_t bcd_usb;
    uint8_t  dev_class;
    uint8_t  dev_sub;
    uint8_t  dev_proto;
    uint8_t  intf_class;
    uint8_t  intf_sub;
    uint8_t  intf_proto;
    uint8_t  ep0_mps;
    uint8_t  n_configs;
    uint8_t  parent_slot;
    uint8_t  parent_port;
    uint8_t  depth;
    uint8_t  present;
    uint32_t route_string;
    char     product[40];
} xhci_pub_dev_t;

typedef struct __attribute__((packed)) {
    uint64_t parameter;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

_Static_assert(sizeof(xhci_trb_t) == 16, "xhci_trb size");

typedef struct __attribute__((packed)) {
    uint64_t ring_segment_base;
    uint16_t ring_segment_size;
    uint16_t reserved16;
    uint32_t reserved32;
} xhci_erst_entry_t;

_Static_assert(sizeof(xhci_erst_entry_t) == 16, "xhci_erst_entry size");

struct xhci_controller {
    void              *pdev;
    volatile uint8_t  *bar0;
    uint32_t           bar_size;

    uint8_t            caplength;
    uint16_t           hciversion;
    uint32_t           hcsparams1;
    uint32_t           hcsparams2;
    uint32_t           hcsparams3;
    uint32_t           hccparams1;
    uint32_t           hccparams2;
    uint32_t           dboff;
    uint32_t           rtsoff;

    uint8_t            max_slots;
    uint16_t           max_intrs;
    uint8_t            max_ports;
    uint32_t           max_scratchpad;

    volatile uint8_t  *op_regs;
    volatile uint8_t  *runtime_regs;
    volatile uint32_t *doorbells;

    uint64_t          *dcbaa;
    uintptr_t          dcbaa_phys;

    uint64_t          *scratchpad_array;
    uintptr_t          scratchpad_array_phys;

    xhci_trb_t        *cmd_ring;
    uintptr_t          cmd_ring_phys;
    uint16_t           cmd_enqueue;
    uint8_t            cmd_cycle;

    xhci_trb_t        *evt_ring;
    uintptr_t          evt_ring_phys;
    uint16_t           evt_dequeue;
    uint8_t            evt_cycle;

    xhci_erst_entry_t *erst;
    uintptr_t          erst_phys;

    bool               ready;
    bool               running;

    int                irq_vector;
    bool               irq_enabled;

    volatile uint32_t  drain_lock;
};

static inline uint32_t xhci_context_size(const xhci_controller_t *c) {
    return (c->hccparams1 & (1u << 2)) ? 64u : 32u;
}

static inline uint32_t *xhci_input_context(const xhci_controller_t *c, void *base, unsigned dci) {
    return (uint32_t *)((uint8_t *)base + (dci + 1) * xhci_context_size(c));
}

#define TRB_CYCLE             (1u << 0)
#define TRB_TC                (1u << 1)
#define TRB_IOC               (1u << 5)
#define TRB_IDT               (1u << 6)
#define TRB_DIR_IN            (1u << 16)

#define TRB_TYPE_SHIFT        10
#define TRB_TYPE(t)           ((uint32_t)(t) << TRB_TYPE_SHIFT)
#define TRB_TYPE_GET(c)       (((c) >> TRB_TYPE_SHIFT) & 0x3F)

#define TRB_NORMAL             1
#define TRB_SETUP              2
#define TRB_DATA               3
#define TRB_STATUS             4
#define TRB_LINK               6
#define TRB_NO_OP_CMD          23
#define TRB_ENABLE_SLOT        9
#define TRB_ADDRESS_DEVICE     11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT   13
#define TRB_TRANSFER_EVT       32
#define TRB_CMD_COMPLETION_EVT 33
#define TRB_PORT_STATUS_EVT    34

#define TRB_TRT_NO_DATA        0
#define TRB_TRT_OUT_DATA       2
#define TRB_TRT_IN_DATA        3

#define CC_SUCCESS             1

#define EP_TYPE_ISOCH_OUT      1
#define EP_TYPE_BULK_OUT       2
#define EP_TYPE_INTR_OUT       3
#define EP_TYPE_CONTROL        4
#define EP_TYPE_ISOCH_IN       5
#define EP_TYPE_BULK_IN        6
#define EP_TYPE_INTR_IN        7

#define USB_GET_DESCRIPTOR     6
#define USB_SET_CONFIGURATION  9
#define USB_DESC_DEVICE        1
#define USB_DESC_CONFIGURATION 2
#define USB_DESC_INTERFACE     4
#define USB_DESC_ENDPOINT      5

typedef struct {
    uint8_t  speed;
    uint8_t  root_hub_port;
    uint32_t route_string;
    uint8_t  parent_slot;
    uint8_t  parent_port;
    uint8_t  depth;
} xhci_topology_t;

typedef struct {
    bool     found;
    bool     is_mouse;
    uint8_t  intf;
    uint8_t  ep_addr;
    uint16_t ep_mps;
    uint8_t  ep_interval;
} usb_kbd_match_t;

typedef struct {
    bool     found;
    uint8_t  intf;
    uint8_t  in_addr, out_addr;
    uint16_t in_mps,  out_mps;
} usb_msc_match_t;

void xhci_init(void);
void xhci_start_worker(void);
int  xhci_controller_count(void);
xhci_controller_t *xhci_get_controller(int idx);

int  xhci_list_devs(xhci_pub_dev_t *out, int max);

int  xhci_send_cmd(xhci_controller_t *c, uint64_t param, uint32_t status,
                   uint32_t control, xhci_trb_t *out_ev);
int  xhci_control_xfer(xhci_controller_t *c, uint8_t slot_id,
                       xhci_trb_t *ep0_ring, uintptr_t ep0_ring_phys,
                       uint16_t *enq, uint8_t *cyc,
                       uint8_t bmRequestType, uint8_t bRequest,
                       uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                       void *data_out);
void xhci_drain_events(xhci_controller_t *c);

int  xhci_hcd_register_usb_dev(xhci_controller_t *c, uint8_t slot_id,
                               const xhci_topology_t *topo,
                               uint16_t vid, uint16_t pid,
                               uint16_t bcd_usb, uint16_t bcd_dev,
                               uint8_t dev_class, uint8_t dev_sub, uint8_t dev_proto,
                               uint8_t intf_class, uint8_t intf_sub, uint8_t intf_proto,
                               uint8_t ep0_mps, uint8_t n_cfg);
int  xhci_hcd_find_root_dev(xhci_controller_t *c, uint8_t root_port);
int  xhci_hcd_find_hub_child(xhci_controller_t *c, uint32_t hub_route,
                             uint8_t hub_depth, uint8_t hub_port, uint8_t root_hub_port);
uint8_t xhci_hcd_slot_at_index(int idx);
void xhci_hcd_disconnect_subtree(uint8_t root_slot);

int  xhci_hid_kbd_register(xhci_controller_t *c, uint8_t slot_id,
                           uint8_t port_id, uint8_t speed,
                           xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                           uint16_t *enq, uint8_t *cyc,
                           const usb_kbd_match_t *m);
void xhci_hid_kbd_tick(void);
int  xhci_hid_kbd_active_count(void);
void xhci_hid_kbd_disconnect_slot(uint8_t slot_id);
bool xhci_hid_kbd_handle_xfer_event(uint8_t slot_id, uint8_t dci, uint8_t cc);

int  xhci_reset_endpoint(xhci_controller_t *c, uint8_t slot_id, uint8_t dci);
int  xhci_set_tr_dequeue(xhci_controller_t *c, uint8_t slot_id, uint8_t dci,
                         uintptr_t deq_phys, uint8_t dcs);

int  xhci_msc_register(xhci_controller_t *c, uint8_t slot_id,
                       uint8_t port_id, uint8_t speed,
                       const usb_msc_match_t *match);
void xhci_msc_post_init(void);
void xhci_msc_disconnect_slot(uint8_t slot_id);
bool xhci_msc_handle_xfer_event(uint8_t slot_id, uint8_t dci, uint8_t cc,
                                uint32_t resid, uint64_t param);

int  xhci_hub_register(xhci_controller_t *c, uint8_t slot_id,
                       const xhci_topology_t *topo,
                       xhci_trb_t *ep0_ring, uintptr_t ep0_phys,
                       uint16_t *enq, uint8_t *cyc);
void xhci_hub_post_init(void);
void xhci_hub_tick(void);
void xhci_hub_disconnect_slot(uint8_t slot_id);

void enumerate_device(xhci_controller_t *c, const xhci_topology_t *topo,
                      const char *path_label);

#endif
