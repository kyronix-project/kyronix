#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../../arch/x86_64/cpu.h"

#define USB_CLASS_HUB 0x09
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KBD 0x01
#define USB_PROTOCOL_MOUSE 0x02
#define USB_SUBCLASS_SCSI 0x06
#define USB_PROTOCOL_BOT 0x50

#define USB_REQ_GET_STATUS 0x00
#define USB_REQ_CLEAR_FEATURE 0x01
#define USB_REQ_SET_FEATURE 0x03
#define USB_REQ_SET_ADDRESS 0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE 0x0A
#define USB_REQ_SET_INTERFACE 0x0B
#define USB_REQ_SYNCH_FRAME 0x0C
#define USB_REQ_SET_IDLE 0x0A
#define USB_REQ_SET_PROTOCOL 0x0B
#define USB_REQ_SET_REPORT 0x09

#define USB_DT_DEVICE 0x01
#define USB_DT_CONFIG 0x02
#define USB_DT_STRING 0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT 0x05
#define USB_DT_DEVICE_QUALIFIER 0x06
#define USB_DT_OTHER_SPEED 0x07
#define USB_DT_HUB 0x29

#define USB_REQTYPE_DIR_IN 0x80
#define USB_REQTYPE_TYPE_CLASS 0x20
#define USB_REQTYPE_RECIP_DEVICE 0x00
#define USB_REQTYPE_RECIP_INTERFACE 0x01
#define USB_REQTYPE_RECIP_ENDPOINT 0x02
#define USB_REQTYPE_RECIP_OTHER 0x03

#define USB_FEATURE_ENDPOINT_HALT 0x00
#define USB_FEATURE_PORT_RESET 4
#define USB_FEATURE_PORT_POWER 8
#define USB_FEATURE_C_PORT_CONNECTION 16
#define USB_FEATURE_C_PORT_RESET 20

#define USB_SPEED_LOW 0
#define USB_SPEED_FULL 1
#define USB_SPEED_HIGH 2
#define USB_SPEED_SUPER 3

#define USB_MAX_DEVICES 64
#define USB_MAX_ENDPOINTS 8
#define USB_MAX_INTERFACES 8
#define USB_MAX_HUB_PORTS 16

#define USB_STALL (-2)
#define USB_TIMEOUT (-3)
#define USB_BABBLE (-4)
#define USB_NAK_LIMIT 8000

typedef struct PACKED {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} usb_device_desc_t;

typedef struct PACKED {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} usb_config_desc_t;

typedef struct PACKED {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} usb_interface_desc_t;

typedef struct PACKED {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
} usb_endpoint_desc_t;

typedef struct PACKED {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_pkt_t;

typedef struct usb_endpoint {
    uint8_t addr;
    uint8_t attributes;
    uint16_t max_packet;
    uint8_t interval;
    uint8_t toggle;
} usb_endpoint_t;

typedef struct usb_interface {
    uint8_t number;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    usb_endpoint_t eps[USB_MAX_ENDPOINTS];
    int num_eps;
    void *drv_data;
} usb_interface_t;

struct usb_hc;

typedef struct usb_device {
    int slot;
    int port;
    int speed;
    int addr;
    int max_packet0;
    struct usb_hc *hc;
    struct usb_device *parent;
    usb_device_desc_t desc;
    usb_interface_t ifaces[USB_MAX_INTERFACES];
    int num_ifaces;
    bool configured;
} usb_device_t;

typedef struct usb_hc_ops {
    int (*reset_port)(struct usb_hc *hc, int port);
    int (*control)(struct usb_hc *hc, usb_device_t *dev, const usb_setup_pkt_t *setup,
                   void *buf, int len, int *actual);
    int (*bulk)(struct usb_hc *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                void *buf, int len, int *actual);
    int (*interrupt)(struct usb_hc *hc, usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle,
                     void *buf, int len, int *actual);
} usb_hc_ops_t;

typedef struct usb_hc {
    const char *name;
    usb_hc_ops_t *ops;
    void *priv;
    int num_ports;
    struct usb_hc *next;
} usb_hc_t;

void usb_init(void);
bool usb_ready(void);
int usb_device_count(void);
usb_device_t *usb_get_device(int idx);

int usb_control_msg(usb_device_t *dev, uint8_t reqtype, uint8_t request, uint16_t value,
                    uint16_t index, void *buf, int len, int *actual);
int usb_get_descriptor(usb_device_t *dev, uint8_t type, uint8_t index, void *buf, int len);
int usb_set_address(usb_device_t *dev, int addr);
int usb_set_configuration(usb_device_t *dev, int config);
int usb_clear_halt(usb_device_t *dev, uint8_t ep_addr);
int usb_bulk_transfer(usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle, void *buf, int len,
                      int *actual);
int usb_interrupt_transfer(usb_device_t *dev, uint8_t ep_addr, uint8_t *toggle, void *buf,
                           int len, int *actual);
void usb_msleep(uint32_t ms);

void usb_hc_register(usb_hc_t *hc);
void usb_enumerate_all(void);

void usbhid_init(void);
void usbhid_probe(usb_device_t *dev, usb_interface_t *iface);
void usbms_init(void);
void usbms_probe(usb_device_t *dev, usb_interface_t *iface);
void usbrndis_probe(usb_device_t *dev, usb_interface_t *iface);
void usbcdcecm_probe(usb_device_t *dev, usb_interface_t *iface);
void usbcdcacm_probe(usb_device_t *dev, usb_interface_t *iface);
void usbnet_asix_probe(usb_device_t *dev, usb_interface_t *iface);
void usbnet_rtl8152_probe(usb_device_t *dev, usb_interface_t *iface);
void rtl8188eu_probe(usb_device_t *dev, usb_interface_t *iface);

void uhci_init(void);
void ohci_init(void);
void ehci_init(void);
void xhci_init(void);
