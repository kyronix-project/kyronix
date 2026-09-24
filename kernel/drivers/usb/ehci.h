#ifndef KERNEL_DRIVERS_EHCI_H
#define KERNEL_DRIVERS_EHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "xhci.h"
#include "usb_hid.h"

#define EHCI_MAX_CONTROLLERS 2

#define EHCI_CAP_CAPLENGTH    0x00
#define EHCI_CAP_HCIVERSION   0x02
#define EHCI_CAP_HCSPARAMS    0x04
#define EHCI_CAP_HCCPARAMS    0x08

#define EHCI_OP_USBCMD        0x00
#define EHCI_OP_USBSTS        0x04
#define EHCI_OP_USBINTR       0x08
#define EHCI_OP_FRINDEX       0x0C
#define EHCI_OP_CTRLDSSEG     0x10
#define EHCI_OP_PERIODICLIST  0x14
#define EHCI_OP_ASYNCLIST     0x18
#define EHCI_OP_CONFIGFLAG    0x40
#define EHCI_OP_PORTSC(n)    (0x44 + (n) * 4)

#define USBCMD_RUN            (1u << 0)
#define USBCMD_HCRESET        (1u << 1)
#define USBCMD_PSE            (1u << 4)
#define USBCMD_ASE            (1u << 5)
#define USBCMD_IAAD           (1u << 6)

#define USBSTS_HCHALTED       (1u << 12)
#define USBSTS_ASYNC          (1u << 15)
#define USBSTS_IAA            (1u << 5)

#define CONFIGFLAG_CF         (1u << 0)

#define PORTSC_CONNECT        (1u << 0)
#define PORTSC_CONNECT_CHG    (1u << 1)
#define PORTSC_ENABLE         (1u << 2)
#define PORTSC_ENABLE_CHG     (1u << 3)
#define PORTSC_OCC            (1u << 4)
#define PORTSC_OCC_CHG        (1u << 5)
#define PORTSC_RESUME         (1u << 6)
#define PORTSC_SUSPEND        (1u << 7)
#define PORTSC_RESET          (1u << 8)
#define PORTSC_PORT_POWER     (1u << 12)
#define PORTSC_OWNER          (1u << 13)
#define PORTSC_LS_MASK        (0x3u << 10)
#define PORTSC_LS_KSTATE      (0x1u << 10)

#define EECP_LEGSUP_CAPID     0x01
#define EECP_LEGSUP_BIOS_OWN  (1u << 16)
#define EECP_LEGSUP_OS_OWN    (1u << 24)

#define QH_T                  1u
#define QTD_T                 1u
#define QH_TYPE_QH            (1u << 1)

#define QTD_STATUS_ACTIVE     (1u << 7)
#define QTD_STATUS_HALTED     (1u << 6)
#define QTD_STATUS_DBE        (1u << 5)
#define QTD_STATUS_BABBLE     (1u << 4)
#define QTD_STATUS_XACTERR    (1u << 3)
#define QTD_STATUS_MISSED     (1u << 2)

#define QTD_PID_OUT           (0u << 8)
#define QTD_PID_IN            (1u << 8)
#define QTD_PID_SETUP         (2u << 8)

#define QTD_CERR_3            (3u << 10)
#define QTD_IOC               (1u << 15)
#define QTD_TBT_SHIFT         16
#define QTD_DT                (1u << 31)

#define EHCI_SPEED_FS         0
#define EHCI_SPEED_LS         1
#define EHCI_SPEED_HS         2

struct __attribute__((aligned(32))) ehci_qtd {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
    uint32_t ext_buf[5];
    uint32_t pad[3];
};

struct __attribute__((aligned(32))) ehci_qh {
    uint32_t hlp;
    uint32_t ep_chars;
    uint32_t ep_caps;
    uint32_t cur_qtd;

    uint32_t overlay_next;
    uint32_t overlay_alt_next;
    uint32_t overlay_token;
    uint32_t overlay_buf[5];
    uint32_t overlay_ext_buf[5];

    uint32_t pad[4];
};

typedef struct ehci_qh ehci_qh_t;
typedef struct ehci_qtd ehci_qtd_t;

typedef struct ehci_controller {
    void              *pdev;
    volatile uint8_t  *bar0;
    uint32_t           bar_size;

    uint8_t            caplength;
    uint16_t           hciversion;
    uint32_t           hcsparams;
    uint32_t           hccparams;
    uint8_t            eecp;

    uint8_t            n_ports;
    bool               port_power_control;
    bool               has_64bit;
    bool               bios_owned_before;
    bool               os_owned;

    volatile uint8_t  *op_regs;

    uint32_t          *periodic_list;
    uintptr_t          periodic_phys;

    ehci_qh_t         *async_head;
    uintptr_t          async_head_phys;

    spinlock_t         async_lock;

    bool               ready;
    bool               running;
} ehci_controller_t;

typedef struct {
    bool     is_msc;
    uint8_t  intf;
    uint8_t  in_ep, out_ep;
    uint16_t in_mps, out_mps;
} ehci_msc_info_t;

typedef struct {
    bool     is_kbd;
    bool     is_mouse;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  interval;
} ehci_hid_info_t;

void ehci_init(void);
void ehci_start_worker(void);
int  ehci_controller_count(void);
ehci_controller_t *ehci_get_controller(int idx);

int ehci_list_devs(xhci_pub_dev_t *out, int max, uint8_t bus_offset);

uint32_t op_r32(ehci_controller_t *c, uint32_t off);
void     op_w32(ehci_controller_t *c, uint32_t off, uint32_t v);

int  ehci_async_pause(ehci_controller_t *c, uint32_t *old_cmd);
void ehci_async_resume(ehci_controller_t *c, uint32_t old_cmd);

ehci_qtd_t *alloc_qtd(uintptr_t *out_phys);
void        qtd_set_buf(ehci_qtd_t *t, uintptr_t buf_phys, uint32_t len);

int  ehci_control_xfer(ehci_controller_t *c,
                       uint8_t addr, uint8_t speed, uint16_t ep0_mps,
                       const uint8_t setup[8],
                       void *data, uint16_t len, bool data_in);

int  ehci_hid_kbd_setup(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                        const ehci_hid_info_t *info);
void ehci_hid_kbd_tick(void);
int  ehci_hid_kbd_active_count(void);
void ehci_hid_kbd_deactivate_addr(uint8_t addr);

int  ehci_msc_setup(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                    const ehci_msc_info_t *info);
void ehci_msc_deactivate_addr(uint8_t addr);

int  ehci_hub_setup(ehci_controller_t *c, uint8_t addr,
                    uint8_t root_port, uint8_t parent_addr,
                    uint8_t parent_port, uint8_t depth,
                    uint32_t route_string);
void ehci_hub_deactivate_addr(uint8_t addr);

uint8_t ehci_alloc_addr(void);

void ehci_finalize_device(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                          const char *label,
                          uint8_t root_port, uint8_t parent_addr,
                          uint8_t parent_port, uint8_t depth,
                          uint32_t route_string);

#endif
