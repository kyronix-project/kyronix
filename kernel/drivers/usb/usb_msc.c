#include "usb_glue.h"
#include "usb_msc.h"
#include "../lib/string.h"

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t  flags;
    uint8_t  lun;
    uint8_t  cb_length;
    uint8_t  cb[16];
} usb_msc_cbw_t;
_Static_assert(sizeof(usb_msc_cbw_t) == 31, "cbw size");

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_residue;
    uint8_t  status;
} usb_msc_csw_t;
_Static_assert(sizeof(usb_msc_csw_t) == 13, "csw size");

static int bulk(usb_msc_dev_t *d, bool in, void *virt, uintptr_t phys,
                uint32_t len, uint32_t timeout) {
    return d->ops.bulk(d->ops.dev, in, virt, phys, len, timeout);
}

static void clear_halt(usb_msc_dev_t *d, bool in) {
    if (d->ops.clear_halt) d->ops.clear_halt(d->ops.dev, in);
}

int usb_msc_scsi(usb_msc_dev_t *d, const uint8_t *cdb, uint8_t cdb_len,
                 bool data_in, void *virt, uintptr_t phys, uint32_t data_len,
                 uint8_t *scsi_status)
{
    if (cdb_len == 0 || cdb_len > 16) return -EINVAL;

    uintptr_t cbw_phys, csw_phys;
    usb_msc_cbw_t *cbw = (usb_msc_cbw_t *)dma_alloc_coherent_low(64, &cbw_phys);
    if (!cbw || cbw_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    usb_msc_csw_t *csw = (usb_msc_csw_t *)dma_alloc_coherent_low(64, &csw_phys);
    if (!csw || csw_phys >= 0xFFFFFFFFULL) { dma_free_coherent(cbw, 64); return -ENOMEM; }

    memset(cbw, 0, sizeof(*cbw));
    memset(csw, 0, sizeof(*csw));

    uint32_t tag = ++d->tag;
    cbw->signature   = USB_MSC_CBW_SIGNATURE;
    cbw->tag         = tag;
    cbw->data_length = data_len;
    cbw->flags       = data_in ? 0x80 : 0x00;
    cbw->lun         = 0;
    cbw->cb_length   = cdb_len;
    memcpy(cbw->cb, cdb, cdb_len);

    uint32_t to = d->timeout_ms ? d->timeout_ms : 30000;

    int r = bulk(d, false, cbw, cbw_phys, sizeof(*cbw), 5000);
    if (r < 0) { clear_halt(d, false); goto out; }

    if (data_len > 0 && (virt || phys)) {
        r = bulk(d, data_in, virt, phys, data_len, to);
        if (r < 0) { clear_halt(d, data_in); goto out; }
    }

    r = bulk(d, true, csw, csw_phys, sizeof(*csw), to);
    if (r < 0) { clear_halt(d, true); goto out; }

    if (csw->signature != USB_MSC_CSW_SIGNATURE || csw->tag != tag) { r = -EIO; goto out; }
    if (scsi_status) *scsi_status = csw->status;
    r = 0;
out:
    dma_free_coherent(cbw, 64);
    dma_free_coherent(csw, 64);
    return r;
}

int usb_msc_inquiry(usb_msc_dev_t *d) {
    uintptr_t bp;
    uint8_t *buf = (uint8_t *)dma_alloc_coherent_low(64, &bp);
    if (!buf || bp >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(buf, 0, 64);
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    uint8_t st = 0xFF;
    int r = usb_msc_scsi(d, cdb, 6, true, buf, bp, 36, &st);
    if (r == 0 && st == 0) {
        memcpy(d->vendor,  buf + 8,  8);  d->vendor[8]  = 0;
        memcpy(d->product, buf + 16, 16); d->product[16] = 0;
        for (int i = 7;  i >= 0 && d->vendor[i]  == ' '; i--) d->vendor[i]  = 0;
        for (int i = 15; i >= 0 && d->product[i] == ' '; i--) d->product[i] = 0;
    } else {
        r = (r < 0) ? r : -EIO;
    }
    dma_free_coherent(buf, 64);
    return r;
}

int usb_msc_test_unit_ready(usb_msc_dev_t *d) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 5; i++) {
        uint8_t st = 0xFF;
        int r = usb_msc_scsi(d, cdb, 6, true, 0, 0, 0, &st);
        if (r == 0 && st == 0) return 0;
        if (st == 2) {
            uintptr_t rp;
            uint8_t *rsp = (uint8_t *)dma_alloc_coherent_low(64, &rp);
            if (rsp && rp < 0xFFFFFFFFULL) {
                memset(rsp, 0, 64);
                uint8_t rs_cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
                uint8_t rss = 0xFF;
                usb_msc_scsi(d, rs_cdb, 6, true, rsp, rp, 18, &rss);
                dma_free_coherent(rsp, 64);
            }
        }
        usb_msleep(100);
    }
    return -EIO;
}

int usb_msc_read_capacity(usb_msc_dev_t *d) {
    uintptr_t bp;
    uint8_t *buf = (uint8_t *)dma_alloc_coherent_low(64, &bp);
    if (!buf || bp >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(buf, 0, 64);
    uint8_t cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
    uint8_t st = 0xFF;
    int r = usb_msc_scsi(d, cdb, 10, true, buf, bp, 8, &st);
    if (r == 0 && st == 0) {
        uint32_t last_lba = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                            ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
        uint32_t blksz    = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
                            ((uint32_t)buf[6] << 8)  | (uint32_t)buf[7];
        d->lba_count  = (uint64_t)last_lba + 1;
        d->block_size = blksz ? blksz : 512;
    } else {
        r = (r < 0) ? r : -EIO;
    }
    dma_free_coherent(buf, 64);
    return r;
}

int usb_msc_rw10(usb_msc_dev_t *d, uint64_t lba, uint32_t count,
                 void *buf, bool is_write)
{
    if (d->block_size == 0) return -EIO;
    if (count == 0) return 0;
    uint32_t max_sect = d->max_sect ? d->max_sect : 8;
    uint8_t *ubuf = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < count) {
        uint32_t chunk = count - done;
        if (chunk > max_sect) chunk = max_sect;
        uint32_t bytes = chunk * d->block_size;

        uintptr_t dph;
        uint8_t *dbuf = (uint8_t *)dma_alloc_coherent_low(bytes, &dph);
        if (!dbuf || dph >= 0xFFFFFFFFULL) {
            if (dbuf) dma_free_coherent(dbuf, bytes);
            return -ENOMEM;
        }
        if (is_write) memcpy(dbuf, ubuf + (size_t)done * d->block_size, bytes);

        uint64_t lba_cur = lba + done;
        uint8_t cdb[10] = {
            is_write ? 0x2A : 0x28, 0,
            (uint8_t)(lba_cur >> 24), (uint8_t)(lba_cur >> 16),
            (uint8_t)(lba_cur >> 8),  (uint8_t)lba_cur,
            0,
            (uint8_t)(chunk >> 8), (uint8_t)chunk, 0
        };
        uint8_t st = 0xFF;
        int r = -1;
        for (int attempt = 0; attempt < 3; attempt++) {
            st = 0xFF;
            r = usb_msc_scsi(d, cdb, 10, !is_write, dbuf, dph, bytes, &st);
            if (r == 0 && st == 0) break;
            if (r < 0) break;
            usb_msleep(10);
        }
        if (r < 0 || st != 0) {
            dma_free_coherent(dbuf, bytes);
            return (r < 0) ? r : -EIO;
        }
        if (!is_write) memcpy(ubuf + (size_t)done * d->block_size, dbuf, bytes);
        dma_free_coherent(dbuf, bytes);
        done += chunk;
    }
    return 0;
}

int usb_msc_sync_cache(usb_msc_dev_t *d) {
    uint8_t cdb[10] = { 0x35, 0,0,0,0,0,0,0,0,0 };
    uint8_t st = 0xFF;
    int r = usb_msc_scsi(d, cdb, 10, true, 0, 0, 0, &st);
    if (r < 0) return r;
    return st == 0 ? 0 : -EIO;
}
