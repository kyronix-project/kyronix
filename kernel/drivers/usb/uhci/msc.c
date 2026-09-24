#include "../usb_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../../../mm/heap.h"
#include "../../../drivers/block/block.h"
#include "../../../drivers/block/blockdev.h"
#include "../../../fs/partition.h"
#include "../usb_msc.h"
#include "../uhci.h"

#define UHCI_MAX_MSC          4
#define UHCI_MSC_MAX_SECT     8
#define UHCI_MSC_TIMEOUT_MS   30000

typedef struct uhci_msc {
    usb_msc_dev_t mscdev;
    uhci_controller_t *ctl;
    uint8_t  addr;
    uint8_t  intf;
    uint8_t  in_ep, out_ep;
    uint16_t in_mps, out_mps;
    uint8_t  low_speed;
    uint32_t in_dt, out_dt;

    uhci_qh_t *in_qh;
    uintptr_t  in_qh_phys;
    uhci_qh_t *out_qh;
    uintptr_t  out_qh_phys;

    struct block_device blk;
    bool     active;
    bool     ready;
    bool     registered;
    int      slot_idx;
} uhci_msc_t;

static uhci_msc_t g_msc_devs[UHCI_MAX_MSC];
static int        g_msc_count = 0;

static int msc_link_bulk_qh(uhci_msc_t *m, bool in_dir) {
    uintptr_t qh_phys;
    uhci_qh_t *qh = (uhci_qh_t *)dma_alloc_coherent_low(4096, &qh_phys);
    if (!qh || qh_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(qh, 0, sizeof(*qh));
    qh->qhlp = m->ctl->ctrl_qh->qhlp;
    qh->qelp = LP_T;
    __asm__ volatile("" ::: "memory");
    m->ctl->ctrl_qh->qhlp = ((uint32_t)qh_phys) | LP_Q;
    __asm__ volatile("" ::: "memory");
    if (in_dir) { m->in_qh  = qh; m->in_qh_phys  = qh_phys; }
    else        { m->out_qh = qh; m->out_qh_phys = qh_phys; }
    return 0;
}

static int uhci_msc_bulk(void *dev, bool in_dir, void *virt, uintptr_t phys,
                         uint32_t len, uint32_t timeout_ms)
{
    (void)virt;
    uhci_msc_t *m = (uhci_msc_t *)dev;
    uhci_qh_t *qh   = in_dir ? m->in_qh   : m->out_qh;
    uint32_t  *dt   = in_dir ? &m->in_dt  : &m->out_dt;
    uint8_t    ep   = in_dir ? m->in_ep   : m->out_ep;
    uint16_t   mps  = in_dir ? m->in_mps  : m->out_mps;
    uint8_t    pid  = in_dir ? UHCI_PID_IN     : UHCI_PID_OUT;
    uint32_t   ls   = m->low_speed ? TD_LS_DEV : 0;
    if (!qh || mps == 0) return -EIO;

    int n_tds = (len == 0) ? 1 : ((len + mps - 1) / mps);
    if (n_tds > 16) return -EINVAL;
    uhci_td_t *tds[16] = {0};
    uintptr_t  tdp[16] = {0};
    for (int i = 0; i < n_tds; i++) {
        tds[i] = uhci_alloc_td(&tdp[i]);
        if (!tds[i]) {
            for (int j = 0; j < i; j++) uhci_free_td(tds[j]);
            return -ENOMEM;
        }
    }

    uintptr_t cur = phys;
    uint16_t rem = (uint16_t)len;
    uint32_t cur_dt = *dt;
    for (int i = 0; i < n_tds; i++) {
        uint16_t chunk = (rem > mps) ? mps : rem;
        bool last = (i == n_tds - 1);
        tds[i]->link   = last ? LP_T : (tdp[i + 1] | LP_VF);
        tds[i]->status = TD_STATUS_ACTIVE | TD_CERR(3) | TD_SPD | ls
                       | (last ? TD_IOC : 0);
        if (len == 0)
            tds[i]->token = TOKEN_NODATA(cur_dt, ep, m->addr, pid);
        else
            tds[i]->token = TOKEN(chunk, cur_dt, ep, m->addr, pid);
        tds[i]->buffer = (uint32_t)cur;
        cur_dt ^= 1;
        cur    += chunk;
        rem    -= chunk;
    }

    __asm__ volatile("" ::: "memory");
    qh->qelp = (uint32_t)tdp[0];
    __asm__ volatile("" ::: "memory");

    uint64_t deadline = usb_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int r = -EIO;
    uhci_td_t *last_td = tds[n_tds - 1];
    while (1) {
        uint32_t st = last_td->status;
        if (!(st & TD_STATUS_ACTIVE)) {
            r = (st & (TD_STATUS_STALLED | TD_STATUS_BABBLE | TD_STATUS_DBE |
                       TD_STATUS_CRC | TD_STATUS_BITSTUF)) ? -EIO : 0;
            break;
        }
        if (usb_now_ns() > deadline) break;
        __asm__ volatile("pause");
    }

    qh->qelp = LP_T;
    __asm__ volatile("" ::: "memory");

    for (int i = 0; i < n_tds; i++) {
        uint32_t st = tds[i]->status;
        if (st & TD_STATUS_ACTIVE) break;
        if (st & (TD_STATUS_STALLED | TD_STATUS_BABBLE | TD_STATUS_DBE |
                  TD_STATUS_CRC | TD_STATUS_BITSTUF)) { r = -EIO; break; }
    }
    *dt = cur_dt;

    for (int i = 0; i < n_tds; i++) uhci_free_td(tds[i]);
    return r;
}

static void uhci_msc_clear_halt(void *dev, bool in_dir) {
    uhci_msc_t *m = (uhci_msc_t *)dev;
    uint8_t ep = in_dir ? (m->in_ep | 0x80) : m->out_ep;
    uint8_t setup[8] = { 0x02, 0x01, 0x00, 0x00, ep, 0x00, 0x00, 0x00 };
    (void)uhci_control_xfer(m->ctl, m->addr, m->low_speed,
                            m->low_speed ? 8 : 64, setup, NULL, 0, false);
    if (in_dir) m->in_dt = 0; else m->out_dt = 0;
    usb_msleep(10);
}

static int msc_blk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    uhci_msc_t *m = (uhci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, buf, false);
}
static int msc_blk_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    uhci_msc_t *m = (uhci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, (void *)buf, true);
}
static int msc_blk_flush(struct block_device *dev) {
    return usb_msc_sync_cache(&((uhci_msc_t *)dev->priv)->mscdev);
}

static struct block_device_ops g_uhci_msc_blk_ops = {
    .read  = msc_blk_read,
    .write = msc_blk_write,
    .flush = msc_blk_flush,
};

int uhci_msc_setup(uhci_controller_t *c, uint8_t addr, bool low_speed,
                   const uhci_msc_info_t *info)
{
    uhci_msc_t *m = NULL;
    int idx = -1;
    for (int i = 0; i < g_msc_count; i++) {
        if (!g_msc_devs[i].active) { m = &g_msc_devs[i]; idx = i; break; }
    }
    if (!m) {
        if (g_msc_count >= UHCI_MAX_MSC) return -ENOMEM;
        idx = g_msc_count++;
        m = &g_msc_devs[idx];
    }
    bool was_registered = m->registered;
    memset(m, 0, sizeof(*m));
    m->ctl       = c;
    m->addr      = addr;
    m->low_speed = low_speed ? 1 : 0;
    m->intf      = info->intf;
    m->in_ep     = info->in_ep;
    m->out_ep    = info->out_ep;
    m->in_mps    = info->in_mps ? info->in_mps : (low_speed ? 8 : 64);
    m->out_mps   = info->out_mps ? info->out_mps : (low_speed ? 8 : 64);
    m->slot_idx  = idx;

    m->mscdev.ops.dev        = m;
    m->mscdev.ops.bulk       = uhci_msc_bulk;
    m->mscdev.ops.clear_halt = uhci_msc_clear_halt;
    m->mscdev.max_sect       = UHCI_MSC_MAX_SECT;
    m->mscdev.timeout_ms     = UHCI_MSC_TIMEOUT_MS;

    if (msc_link_bulk_qh(m, true)  < 0) return -ENOMEM;
    if (msc_link_bulk_qh(m, false) < 0) return -ENOMEM;

    m->active = true;

    if (usb_msc_inquiry(&m->mscdev) < 0) {
        serial_printf("[uhci-msc] INQUIRY failed addr=%u\n", addr);
        return -EIO;
    }
    if (usb_msc_test_unit_ready(&m->mscdev) < 0) {
        serial_printf("[uhci-msc] TEST_UNIT_READY failed addr=%u\n", addr);
        return -EIO;
    }
    if (usb_msc_read_capacity(&m->mscdev) < 0) {
        serial_printf("[uhci-msc] READ_CAPACITY failed addr=%u\n", addr);
        return -EIO;
    }

    snprintf(m->blk.name, sizeof(m->blk.name), "uhd%d", idx);
    m->blk.sectors     = m->mscdev.lba_count;
    m->blk.sector_size = m->mscdev.block_size;
    m->blk.ops         = &g_uhci_msc_blk_ops;
    m->blk.priv        = m;
    m->blk.offset_lba  = 0;
    m->blk.parent      = NULL;

    if (!was_registered) {
        block_register(&m->blk);
        blockdev_create_node(&m->blk);
    }
    m->registered = true;
    m->ready = true;
    serial_printf("[uhci-msc] /dev/%s: %s %s - %llu sectors x %u (%llu MB)\n",
                  m->blk.name, m->mscdev.vendor, m->mscdev.product,
                  (unsigned long long)m->mscdev.lba_count, m->mscdev.block_size,
                  (unsigned long long)(m->mscdev.lba_count * (uint64_t)m->mscdev.block_size / (1024 * 1024)));

    partition_rescan_disk(&m->blk);
    return 0;
}

void uhci_msc_deactivate_addr(uint8_t addr) {
    for (int j = 0; j < g_msc_count; j++) {
        if (g_msc_devs[j].active && g_msc_devs[j].addr == addr) {
            g_msc_devs[j].active = false;
            g_msc_devs[j].ready  = false;

            for (int k = block_count() - 1; k >= 0; k--) {
                struct block_device *b = block_get(k);
                if (b && (b == &g_msc_devs[j].blk || b->parent == &g_msc_devs[j].blk)) {
                    blockdev_remove_node(b);
                    block_unregister(b);
                    if (b != &g_msc_devs[j].blk) kfree(b);
                }
            }
            g_msc_devs[j].registered = false;
            serial_printf("[uhci-hp] /dev/%s removed\n", g_msc_devs[j].blk.name);
        }
    }
}
