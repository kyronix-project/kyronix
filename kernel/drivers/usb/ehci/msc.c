#include "../usb_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../../../mm/heap.h"
#include "../../../drivers/block/block.h"
#include "../../../drivers/block/blockdev.h"
#include "../../../fs/partition.h"
#include "../usb_msc.h"
#include "../ehci.h"

#define EHCI_MAX_MSC          4
#define EHCI_MSC_MAX_SECT     8

typedef struct ehci_msc {
    usb_msc_dev_t mscdev;
    ehci_controller_t *ctl;
    uint8_t   addr;
    uint8_t   speed;
    uint8_t   intf;
    uint8_t   in_ep, out_ep;
    uint16_t  in_mps, out_mps;
    uint32_t  in_dt_bit, out_dt_bit;

    ehci_qh_t *in_qh, *out_qh;
    uintptr_t  in_qh_phys, out_qh_phys;

    struct block_device blk;
    bool      active;
    bool      ready;
    bool      registered;
    int       slot_idx;
} ehci_msc_t;

static ehci_msc_t g_msc_devs[EHCI_MAX_MSC];
static int g_msc_count = 0;

static int link_bulk_qh(ehci_msc_t *m, bool in_dir) {
    uintptr_t qh_phys;
    ehci_qh_t *qh = (ehci_qh_t *)dma_alloc_coherent_low(4096, &qh_phys);
    if (!qh || qh_phys >= 0xFFFFFFFFULL) return -ENOMEM;
    memset(qh, 0, sizeof(*qh));

    uint8_t  ep_num = in_dir ? m->in_ep : m->out_ep;
    uint16_t mps    = in_dir ? m->in_mps : m->out_mps;
    uint32_t c_flag = (m->speed == EHCI_SPEED_HS) ? 0u : (1u << 27);

    qh->ep_chars = ((uint32_t)m->addr & 0x7F)
                 | ((uint32_t)ep_num << 8)
                 | ((uint32_t)m->speed << 12)
                 | ((uint32_t)mps << 16)
                 | c_flag
                 | (0u << 28);
    qh->ep_caps  = (1u << 30);
    qh->cur_qtd  = 0;
    qh->overlay_next  = QTD_T;
    qh->overlay_alt_next = QTD_T;
    qh->overlay_token = QTD_STATUS_HALTED;

    uint32_t old_cmd;
    if (ehci_async_pause(m->ctl, &old_cmd) < 0) {
        dma_free_coherent(qh, 4096);
        return -EIO;
    }
    qh->hlp = m->ctl->async_head->hlp;
    __asm__ volatile("" ::: "memory");
    m->ctl->async_head->hlp = ((uint32_t)qh_phys) | QH_TYPE_QH;
    ehci_async_resume(m->ctl, old_cmd);

    if (in_dir) { m->in_qh = qh;  m->in_qh_phys  = qh_phys; }
    else        { m->out_qh = qh; m->out_qh_phys = qh_phys; }
    return 0;
}

static int ehci_msc_bulk(void *dev, bool dir_in, void *virt, uintptr_t buf_phys,
                         uint32_t len, uint32_t timeout_ms)
{
    (void)virt;
    ehci_msc_t *m = (ehci_msc_t *)dev;
    ehci_qh_t *qh    = dir_in ? m->in_qh   : m->out_qh;
    uint32_t  *dt_ptr= dir_in ? &m->in_dt_bit : &m->out_dt_bit;
    if (!qh) return -EIO;

    uintptr_t tp;
    ehci_qtd_t *t = alloc_qtd(&tp);
    if (!t) return -ENOMEM;
    t->next     = QTD_T;
    t->alt_next = QTD_T;
    t->token    = QTD_STATUS_ACTIVE | QTD_CERR_3 | QTD_IOC
                | ((uint32_t)len << QTD_TBT_SHIFT)
                | (dir_in ? QTD_PID_IN : QTD_PID_OUT);
    qtd_set_buf(t, buf_phys, len);

    uint32_t old_cmd;
    if (ehci_async_pause(m->ctl, &old_cmd) < 0) {
        dma_free_coherent(t, 4096);
        return -EIO;
    }
    qh->cur_qtd          = 0;
    qh->overlay_next     = (uint32_t)tp;
    qh->overlay_alt_next = QTD_T;
    qh->overlay_token    = *dt_ptr;
    qh->overlay_buf[0]   = 0;
    qh->overlay_buf[1]   = 0;
    qh->overlay_buf[2]   = 0;
    qh->overlay_buf[3]   = 0;
    qh->overlay_buf[4]   = 0;
    __asm__ volatile("" ::: "memory");
    ehci_async_resume(m->ctl, old_cmd);

    uint64_t deadline = usb_now_ns() + (uint64_t)timeout_ms * 1000000ULL;
    int r = -EIO;
    for (;;) {
        uint32_t st = t->token & 0xFF;
        if (!(st & QTD_STATUS_ACTIVE)) {
            if (st & (QTD_STATUS_HALTED | QTD_STATUS_DBE | QTD_STATUS_BABBLE |
                      QTD_STATUS_XACTERR | QTD_STATUS_MISSED)) {
                serial_printf("[ehci-msc] bulk %s ERR token=0x%08x len=%u qh_tok=0x%08x\n",
                       dir_in ? "IN" : "OUT", t->token, len, qh->overlay_token);
                r = -EIO;
            } else {
                r = 0;
            }
            break;
        }
        if (usb_now_ns() > deadline) {
            serial_printf("[ehci-msc] bulk %s TIMEOUT len=%u token=0x%08x qh_tok=0x%08x usbsts=0x%x\n",
                   dir_in ? "IN" : "OUT", len, t->token, qh->overlay_token,
                   op_r32(m->ctl, EHCI_OP_USBSTS));
            break;
        }
        __asm__ volatile("pause");
    }

    *dt_ptr = qh->overlay_token & QTD_DT;

    {
        uint32_t old_cmd2;
        if (ehci_async_pause(m->ctl, &old_cmd2) == 0) {
            qh->cur_qtd          = 0;
            qh->overlay_next     = QTD_T;
            qh->overlay_alt_next = QTD_T;
            qh->overlay_token    = (*dt_ptr) | QTD_STATUS_HALTED;
            __asm__ volatile("" ::: "memory");
            ehci_async_resume(m->ctl, old_cmd2);
        }
    }

    dma_free_coherent(t, 4096);
    return r;
}

static void ehci_msc_clear_halt(void *dev, bool dir_in) {
    (void)dir_in;
    ehci_msc_t *m = (ehci_msc_t *)dev;
    uint8_t clr_in[8]  = { 0x02, 0x01, 0x00, 0x00,
                           (uint8_t)(m->in_ep | 0x80), 0x00, 0x00, 0x00 };
    (void)ehci_control_xfer(m->ctl, m->addr, m->speed, 64, clr_in, NULL, 0, false);

    uint8_t clr_out[8] = { 0x02, 0x01, 0x00, 0x00,
                           m->out_ep, 0x00, 0x00, 0x00 };
    (void)ehci_control_xfer(m->ctl, m->addr, m->speed, 64, clr_out, NULL, 0, false);

    m->in_dt_bit  = 0;
    m->out_dt_bit = 0;
    usb_msleep(20);
    serial_printf("[ehci-msc] clear-halt recovery done (addr=%u)\n", m->addr);
}

static int msc_blk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    ehci_msc_t *m = (ehci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, buf, false);
}
static int msc_blk_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    ehci_msc_t *m = (ehci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, (void *)buf, true);
}
static int msc_blk_flush(struct block_device *dev) {
    ehci_msc_t *m = (ehci_msc_t *)dev->priv;
    if (!m->ready) return 0;
    return usb_msc_sync_cache(&m->mscdev);
}
static struct block_device_ops g_ehci_msc_blk_ops = {
    .read  = msc_blk_read,
    .write = msc_blk_write,
    .flush = msc_blk_flush,
};

int ehci_msc_setup(ehci_controller_t *c, uint8_t addr, uint8_t speed,
                   const ehci_msc_info_t *info)
{
    ehci_msc_t *m = NULL;
    int idx = -1;
    for (int i = 0; i < g_msc_count; i++) {
        if (!g_msc_devs[i].active) { m = &g_msc_devs[i]; idx = i; break; }
    }
    if (!m) {
        if (g_msc_count >= EHCI_MAX_MSC) return -ENOMEM;
        idx = g_msc_count++;
        m = &g_msc_devs[idx];
    }
    bool was_registered = m->registered;
    memset(m, 0, sizeof(*m));
    m->ctl      = c;
    m->addr     = addr;
    m->speed    = speed;
    m->intf     = info->intf;
    m->in_ep    = info->in_ep;
    m->out_ep   = info->out_ep;
    m->in_mps   = info->in_mps;
    m->out_mps  = info->out_mps;
    m->slot_idx = idx;

    m->mscdev.ops.dev        = m;
    m->mscdev.ops.bulk       = ehci_msc_bulk;
    m->mscdev.ops.clear_halt = ehci_msc_clear_halt;
    m->mscdev.max_sect       = EHCI_MSC_MAX_SECT;
    m->mscdev.timeout_ms     = 60000;

    if (link_bulk_qh(m, true)  < 0) return -ENOMEM;
    if (link_bulk_qh(m, false) < 0) return -ENOMEM;

    m->active = true;

    if (usb_msc_inquiry(&m->mscdev) < 0) {
        serial_printf("[ehci-msc] INQUIRY failed for addr=%u\n", addr);
        return -EIO;
    }
    if (usb_msc_test_unit_ready(&m->mscdev) < 0) {
        serial_printf("[ehci-msc] TEST UNIT READY failed for addr=%u\n", addr);
        return -EIO;
    }
    if (usb_msc_read_capacity(&m->mscdev) < 0) {
        serial_printf("[ehci-msc] READ CAPACITY failed for addr=%u\n", addr);
        return -EIO;
    }

    snprintf(m->blk.name, sizeof(m->blk.name), "ehd%d", idx);
    m->blk.sectors     = m->mscdev.lba_count;
    m->blk.sector_size = m->mscdev.block_size;
    m->blk.ops         = &g_ehci_msc_blk_ops;
    m->blk.priv        = m;
    m->blk.offset_lba  = 0;
    m->blk.parent      = NULL;

    if (!was_registered) {
        block_register(&m->blk);
        blockdev_create_node(&m->blk);
    }
    m->registered = true;
    m->ready = true;
    serial_printf("[ehci-msc] /dev/%s: %s %s - %llu sectors x %u (%llu MB)\n",
                  m->blk.name, m->mscdev.vendor, m->mscdev.product,
                  (unsigned long long)m->mscdev.lba_count, m->mscdev.block_size,
                  (unsigned long long)(m->mscdev.lba_count * (uint64_t)m->mscdev.block_size / (1024 * 1024)));

    partition_rescan_disk(&m->blk);
    return 0;
}

void ehci_msc_deactivate_addr(uint8_t addr) {
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
            serial_printf("[ehci-hp] /dev/%s removed\n", g_msc_devs[j].blk.name);
        }
    }
}
