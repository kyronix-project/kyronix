#ifndef KERNEL_DRIVERS_UHCI_H
#define KERNEL_DRIVERS_UHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xhci.h"
#include "usb_hid.h"

#define UHCI_MAX_CONTROLLERS 4

#define UHCI_USBCMD       0x00
#define UHCI_USBSTS       0x02
#define UHCI_USBINTR      0x04
#define UHCI_FRNUM        0x06
#define UHCI_FRBASEADD    0x08
#define UHCI_SOFMOD       0x0C
#define UHCI_PORTSC(n)   (0x10 + (n) * 2)

#define UHCI_CMD_RS         0x0001
#define UHCI_CMD_HCRESET    0x0002
#define UHCI_CMD_GRESET     0x0004
#define UHCI_CMD_EGSM       0x0008
#define UHCI_CMD_FGR        0x0010
#define UHCI_CMD_MAXP       0x0080
#define UHCI_CMD_CF         0x0040

#define UHCI_STS_USBINT     0x0001
#define UHCI_STS_ERROR      0x0002
#define UHCI_STS_HALTED     0x0020

#define UHCI_PSC_CONNECT    0x0001
#define UHCI_PSC_CONN_CHG   0x0002
#define UHCI_PSC_ENABLE     0x0004
#define UHCI_PSC_ENABLE_CHG 0x0008
#define UHCI_PSC_LOWSPEED   0x0100
#define UHCI_PSC_RESET      0x0200
#define UHCI_PSC_SUSPEND    0x1000

#define UHCI_PID_SETUP      0x2D
#define UHCI_PID_IN         0x69
#define UHCI_PID_OUT        0xE1

#define LP_T              0x00000001u
#define LP_Q              0x00000002u
#define LP_VF             0x00000004u

#define TD_STATUS_ACTIVE  (1u << 23)
#define TD_STATUS_STALLED (1u << 22)
#define TD_STATUS_DBE     (1u << 21)
#define TD_STATUS_BABBLE  (1u << 20)
#define TD_STATUS_NAK     (1u << 19)
#define TD_STATUS_CRC     (1u << 18)
#define TD_STATUS_BITSTUF (1u << 17)
#define TD_IOC            (1u << 24)
#define TD_LS_DEV         (1u << 26)
#define TD_CERR(n)        (((uint32_t)(n) & 3u) << 27)
#define TD_SPD            (1u << 29)

#define TD_ACT_LEN(s)     ((s) & 0x7FFu)
#define TD_ACT_LEN_NULL   0x7FFu

#define TOKEN(maxlen, dt, ep, addr, pid) \
    ((((uint32_t)(((uint32_t)(maxlen) - 1u) & 0x7FFu)) << 21) | \
     (((uint32_t)((dt) & 1u)) << 19) | \
     (((uint32_t)((ep)  & 0xFu)) << 15) | \
     (((uint32_t)((addr) & 0x7Fu)) << 8) | \
     ((uint32_t)(pid)))

#define TOKEN_NODATA(dt, ep, addr, pid) \
    ((TD_ACT_LEN_NULL << 21) | \
     (((uint32_t)((dt) & 1u)) << 19) | \
     (((uint32_t)((ep)  & 0xFu)) << 15) | \
     (((uint32_t)((addr) & 0x7Fu)) << 8) | \
     ((uint32_t)(pid)))

struct __attribute__((packed, aligned(16))) uhci_td {
    uint32_t link;
    uint32_t status;
    uint32_t token;
    uint32_t buffer;
    uint32_t sw[4];
};

struct __attribute__((packed, aligned(16))) uhci_qh {
    uint32_t qhlp;
    uint32_t qelp;
    uint32_t sw[2];
};

typedef struct uhci_qh uhci_qh_t;
typedef struct uhci_td uhci_td_t;

typedef struct uhci_controller {
    void    *pdev;
    uint16_t io_base;
    uint8_t  n_ports;

    uint32_t  *frame_list;
    uintptr_t  frame_list_phys;

    uhci_qh_t *ctrl_qh;
    uintptr_t  ctrl_qh_phys;

    bool     ready;
    bool     running;
} uhci_controller_t;

typedef struct {
    bool     is_kbd;
    bool     is_mouse;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  interval;
} uhci_hid_info_t;

typedef struct {
    bool     is_msc;
    uint8_t  intf;
    uint8_t  in_ep, out_ep;
    uint16_t in_mps, out_mps;
} uhci_msc_info_t;

typedef struct {
    bool     is_hub;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  interval;
} uhci_hub_info_t;

void uhci_init(void);
void uhci_start_worker(void);
int  uhci_controller_count(void);
uhci_controller_t *uhci_get_controller(int idx);

int uhci_list_devs(xhci_pub_dev_t *out, int max, uint8_t bus_offset);

uhci_td_t *uhci_alloc_td(uintptr_t *phys);
void       uhci_free_td(uhci_td_t *t);
int        uhci_control_xfer(uhci_controller_t *c, uint8_t addr, bool low_speed,
                             uint16_t mps, const uint8_t setup[8],
                             void *data, uint16_t len, bool data_in);

int  uhci_hid_kbd_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                        const uhci_hid_info_t *info);
void uhci_hid_kbd_tick(void);
int  uhci_hid_kbd_active_count(void);
void uhci_hid_kbd_deactivate_addr(uint8_t addr);

int  uhci_msc_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                    const uhci_msc_info_t *info);
void uhci_msc_deactivate_addr(uint8_t addr);

int  uhci_hub_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                    const uhci_hub_info_t *info);
void uhci_hub_tick(void);
void uhci_hub_deactivate_addr(uint8_t addr);

#endif
