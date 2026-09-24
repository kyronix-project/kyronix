#ifndef KERNEL_DRIVERS_USB_MSC_H
#define KERNEL_DRIVERS_USB_MSC_H

#include <stdint.h>
#include <stdbool.h>
#include "usb_xfer.h"

#define USB_MSC_CBW_SIGNATURE 0x43425355u
#define USB_MSC_CSW_SIGNATURE 0x53425355u

typedef struct {
    usb_xfer_ops_t ops;
    uint32_t tag;
    uint64_t lba_count;
    uint32_t block_size;
    uint32_t max_sect;
    uint32_t timeout_ms;
    char     vendor[9];
    char     product[17];
} usb_msc_dev_t;

int usb_msc_scsi(usb_msc_dev_t *d, const uint8_t *cdb, uint8_t cdb_len,
                 bool data_in, void *virt, uintptr_t phys, uint32_t data_len,
                 uint8_t *scsi_status);

int usb_msc_inquiry(usb_msc_dev_t *d);
int usb_msc_test_unit_ready(usb_msc_dev_t *d);
int usb_msc_read_capacity(usb_msc_dev_t *d);
int usb_msc_rw10(usb_msc_dev_t *d, uint64_t lba, uint32_t count,
                 void *buf, bool is_write);
int usb_msc_sync_cache(usb_msc_dev_t *d);

#endif
