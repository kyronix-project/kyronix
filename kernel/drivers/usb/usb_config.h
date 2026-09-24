#ifndef KERNEL_DRIVERS_USB_CONFIG_H
#define KERNEL_DRIVERS_USB_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     present;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  out_ep;
    uint16_t out_mps;
} usb_msc_iface_t;

typedef struct {
    bool     present;
    bool     is_mouse;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  interval;
} usb_hid_iface_t;

typedef struct {
    bool     present;
    uint8_t  intf;
    uint8_t  in_ep;
    uint16_t in_mps;
    uint8_t  interval;
} usb_hub_iface_t;

#define USB_MAX_HID_IFACES 4

typedef struct {
    usb_msc_iface_t msc;
    usb_hid_iface_t hid;
    usb_hid_iface_t hid_list[USB_MAX_HID_IFACES];
    uint8_t  hid_count;
    usb_hub_iface_t hub;
    uint8_t  cfg_value;
    uint8_t  first_class;
    uint8_t  first_sub;
    uint8_t  first_proto;
} usb_ifaces_t;

void usb_parse_config(const uint8_t *buf, uint16_t total, usb_ifaces_t *out);

#endif
