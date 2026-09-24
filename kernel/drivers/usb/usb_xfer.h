#ifndef KERNEL_DRIVERS_USB_XFER_H
#define KERNEL_DRIVERS_USB_XFER_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    void *dev;
    int (*bulk)(void *dev, bool in, void *virt, uintptr_t phys,
                uint32_t len, uint32_t timeout_ms);
    void (*clear_halt)(void *dev, bool in);
} usb_xfer_ops_t;

#endif
