#include "usb.h"
#include "../../lib/log.h"
#include "../../lib/printf.h"
#include "../../lib/string.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../block.h"

#define CBW_SIGNATURE 0x43425355u
#define CSW_SIGNATURE 0x53425355u
#define CBW_FLAGS_OUT 0x00
#define CBW_FLAGS_IN 0x80

#define SCSI_TEST_UNIT_READY 0x00
#define SCSI_INQUIRY 0x12
#define SCSI_REQUEST_SENSE 0x03
#define SCSI_READ_CAPACITY_10 0x25
#define SCSI_READ_10 0x28
#define SCSI_WRITE_10 0x2A
#define SCSI_MODE_SENSE_6 0x1A

#define USBMS_MAX_DEVS 4
#define USBMS_DMA_PAGES 32
#define USBMS_MAX_SECTORS 32

typedef struct PACKED {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} usbms_cbw_t;

typedef struct PACKED {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} usbms_csw_t;

typedef struct {
    usb_device_t *dev;
    usb_interface_t *iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint8_t *toggle_in;
    uint8_t *toggle_out;
    uint8_t *cbw_buf;
    uint64_t cbw_phys;
    uint8_t *csw_buf;
    uint64_t csw_phys;
    uint8_t *dma;
    uint64_t dma_phys;
    uint32_t tag;
    uint64_t sectors;
    uint32_t sector_size;
    struct block_device bdev;
    char name[BLOCK_NAME_MAX];
} usbms_dev_t;

static usbms_dev_t g_ms[USBMS_MAX_DEVS];
static int g_nms;

static int usbms_bot_xfer(usbms_dev_t *m, const uint8_t *cdb, int cdb_len, void *data,
                          uint32_t data_len, bool data_in) {
    usbms_cbw_t *cbw = (usbms_cbw_t *) m->cbw_buf;
    memset(cbw, 0, sizeof(*cbw));
    cbw->dCBWSignature = CBW_SIGNATURE;
    cbw->dCBWTag = ++m->tag;
    cbw->dCBWDataTransferLength = data_len;
    cbw->bmCBWFlags = data_in ? CBW_FLAGS_IN : CBW_FLAGS_OUT;
    cbw->bCBWLUN = 0;
    cbw->bCBWCBLength = (uint8_t) cdb_len;
    memcpy(cbw->CBWCB, cdb, cdb_len);

    int actual = 0;
    int r = usb_bulk_transfer(m->dev, m->ep_out, m->toggle_out, m->cbw_buf, 31, &actual);
    if (r == USB_STALL) {
        usb_clear_halt(m->dev, m->ep_out);
        r = usb_bulk_transfer(m->dev, m->ep_out, m->toggle_out, m->cbw_buf, 31, &actual);
    }
    if (r || actual != 31) return -1;

    if (data_len > 0) {
        uint8_t ep = data_in ? m->ep_in : m->ep_out;
        uint8_t *tog = data_in ? m->toggle_in : m->toggle_out;
        r = usb_bulk_transfer(m->dev, ep, tog, data, (int) data_len, &actual);
        if (r == USB_STALL) {
            usb_clear_halt(m->dev, ep);
            r = usb_bulk_transfer(m->dev, ep, tog, data, (int) data_len, &actual);
        }
        if (r && r != USB_STALL) return -1;
    }

    usbms_csw_t *csw = (usbms_csw_t *) m->csw_buf;
    memset(csw, 0, sizeof(*csw));
    r = usb_bulk_transfer(m->dev, m->ep_in, m->toggle_in, m->csw_buf, 13, &actual);
    if (r == USB_STALL) {
        usb_clear_halt(m->dev, m->ep_in);
        r = usb_bulk_transfer(m->dev, m->ep_in, m->toggle_in, m->csw_buf, 13, &actual);
    }
    if (r || actual < 13) return -1;
    if (csw->dCSWSignature != CSW_SIGNATURE || csw->dCSWTag != m->tag) return -1;
    if (csw->bCSWStatus != 0) return -(int) csw->bCSWStatus;
    return 0;
}

static int usbms_sense_recover(usbms_dev_t *m) {
    uint8_t cdb[6] = {SCSI_REQUEST_SENSE, 0, 0, 0, 18, 0};
    memset(m->dma, 0, 18);
    return usbms_bot_xfer(m, cdb, 6, m->dma, 18, true);
}

static int usbms_read_sectors(usbms_dev_t *m, uint64_t lba, uint32_t count, void *buf) {
    while (count > 0) {
        uint32_t chunk = count > USBMS_MAX_SECTORS ? USBMS_MAX_SECTORS : count;
        uint32_t bytes = chunk * m->sector_size;

        uint8_t cdb[10] = {0};
        cdb[0] = SCSI_READ_10;
        cdb[2] = (uint8_t) (lba >> 24);
        cdb[3] = (uint8_t) (lba >> 16);
        cdb[4] = (uint8_t) (lba >> 8);
        cdb[5] = (uint8_t) lba;
        cdb[7] = (uint8_t) (chunk >> 8);
        cdb[8] = (uint8_t) chunk;

        int r = usbms_bot_xfer(m, cdb, 10, m->dma, bytes, true);
        if (r) {
            usbms_sense_recover(m);
            r = usbms_bot_xfer(m, cdb, 10, m->dma, bytes, true);
            if (r) return -1;
        }
        memcpy(buf, m->dma, bytes);
        buf = (uint8_t *) buf + bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int usbms_write_sectors(usbms_dev_t *m, uint64_t lba, uint32_t count, const void *buf) {
    while (count > 0) {
        uint32_t chunk = count > USBMS_MAX_SECTORS ? USBMS_MAX_SECTORS : count;
        uint32_t bytes = chunk * m->sector_size;
        memcpy(m->dma, buf, bytes);

        uint8_t cdb[10] = {0};
        cdb[0] = SCSI_WRITE_10;
        cdb[2] = (uint8_t) (lba >> 24);
        cdb[3] = (uint8_t) (lba >> 16);
        cdb[4] = (uint8_t) (lba >> 8);
        cdb[5] = (uint8_t) lba;
        cdb[7] = (uint8_t) (chunk >> 8);
        cdb[8] = (uint8_t) chunk;

        int r = usbms_bot_xfer(m, cdb, 10, m->dma, bytes, false);
        if (r) {
            usbms_sense_recover(m);
            r = usbms_bot_xfer(m, cdb, 10, m->dma, bytes, false);
            if (r) return -1;
        }
        buf = (const uint8_t *) buf + bytes;
        lba += chunk;
        count -= chunk;
    }
    return 0;
}

static int usbms_block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    return usbms_read_sectors((usbms_dev_t *) dev->priv, lba, count, buf);
}

static int usbms_block_write(struct block_device *dev, uint64_t lba, uint32_t count,
                             const void *buf) {
    return usbms_write_sectors((usbms_dev_t *) dev->priv, lba, count, buf);
}

static int usbms_block_flush(struct block_device *dev) {
    (void) dev;
    return 0;
}

static struct block_device_ops g_usbms_ops = {
    .read = usbms_block_read,
    .write = usbms_block_write,
    .flush = usbms_block_flush,
};

void usbms_probe(usb_device_t *dev, usb_interface_t *iface) {
    if (g_nms >= USBMS_MAX_DEVS) return;

    usb_endpoint_t *ep_in = NULL, *ep_out = NULL;
    for (int i = 0; i < iface->num_eps; i++) {
        usb_endpoint_t *e = &iface->eps[i];
        if ((e->attributes & 3) != 2) continue;
        if (e->addr & 0x80) ep_in = e;
        else ep_out = e;
    }
    if (!ep_in || !ep_out) return;

    usbms_dev_t *m = &g_ms[g_nms];
    memset(m, 0, sizeof(*m));
    m->dev = dev;
    m->iface = iface;
    m->ep_in = ep_in->addr;
    m->ep_out = ep_out->addr;
    m->toggle_in = &ep_in->toggle;
    m->toggle_out = &ep_out->toggle;

    m->cbw_phys = (uint64_t) pmm_alloc_zeroed();
    m->csw_phys = (uint64_t) pmm_alloc_zeroed();
    if (!m->cbw_phys || !m->csw_phys) return;
    m->cbw_buf = (uint8_t *) phys_to_virt(m->cbw_phys);
    m->csw_buf = (uint8_t *) phys_to_virt(m->csw_phys);

    m->dma_phys = (uint64_t) pmm_alloc_contiguous(USBMS_DMA_PAGES);
    if (!m->dma_phys) return;
    m->dma = (uint8_t *) phys_to_virt(m->dma_phys);
    memset(m->dma, 0, USBMS_DMA_PAGES * PAGE_SIZE);

    uint8_t cdb_tur[6] = {0};
    if (usbms_bot_xfer(m, cdb_tur, 6, NULL, 0, false)) usbms_sense_recover(m);

    memset(m->dma, 0, 36);
    uint8_t cdb_inq[6] = {SCSI_INQUIRY, 0, 0, 0, 36, 0};
    if (usbms_bot_xfer(m, cdb_inq, 6, m->dma, 36, true)) {
        log_warn("USBMS: INQUIRY failed on addr %d", dev->addr);
        return;
    }
    char vendor[9] = {0}, product[17] = {0};
    memcpy(vendor, m->dma + 8, 8);
    memcpy(product, m->dma + 16, 16);
    for (int i = 7; i >= 0 && vendor[i] == ' '; i--) vendor[i] = 0;
    for (int i = 15; i >= 0 && product[i] == ' '; i--) product[i] = 0;

    memset(m->dma, 0, 8);
    uint8_t cdb_cap[10] = {SCSI_READ_CAPACITY_10, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (usbms_bot_xfer(m, cdb_cap, 10, m->dma, 8, true)) {
        log_warn("USBMS: READ CAPACITY failed on addr %d", dev->addr);
        return;
    }
    uint32_t last_lba = ((uint32_t) m->dma[0] << 24) | ((uint32_t) m->dma[1] << 16) |
                        ((uint32_t) m->dma[2] << 8) | m->dma[3];
    m->sector_size = ((uint32_t) m->dma[4] << 24) | ((uint32_t) m->dma[5] << 16) |
                     ((uint32_t) m->dma[6] << 8) | m->dma[7];
    if (m->sector_size == 0 || m->sector_size > 4096) m->sector_size = 512;
    m->sectors = (uint64_t) last_lba + 1;
    if (!m->sectors) return;

    snprintf(m->name, BLOCK_NAME_MAX, "usb%d", g_nms);
    m->bdev.sectors = m->sectors;
    m->bdev.sector_size = m->sector_size;
    m->bdev.ops = &g_usbms_ops;
    m->bdev.priv = m;
    memcpy(m->bdev.name, m->name, BLOCK_NAME_MAX);
    block_register(&m->bdev);

    log_info("USBMS: %s %s %s - %lu sectors x %u bytes (%lu MiB)", m->name, vendor, product,
             m->sectors, m->sector_size,
             (m->sectors * m->sector_size) / (1024 * 1024));
    g_nms++;
}

void usbms_init(void) { g_nms = 0; }
