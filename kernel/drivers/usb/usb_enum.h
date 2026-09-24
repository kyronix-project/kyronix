#ifndef KERNEL_DRIVERS_USB_ENUM_H
#define KERNEL_DRIVERS_USB_ENUM_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void *handle;
    int (*control)(void *handle, const uint8_t setup[8],
                   void *data, uint16_t len, bool in);
} usb_ctrl_ops_t;

int usb_get_device_descriptor(usb_ctrl_ops_t *o, uint8_t *desc18);
int usb_get_config(usb_ctrl_ops_t *o, uint8_t *cfg_full, uint16_t cap,
                   uint16_t *out_total, uint8_t *out_cfg_value);
int usb_set_configuration(usb_ctrl_ops_t *o, uint8_t cfg_value);

#endif
