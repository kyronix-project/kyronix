#include "../usb_glue.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../../../drivers/block/block.h"
#include "../../../drivers/block/blockdev.h"
#include "../../../fs/partition.h"
#include "../../../mm/heap.h"
#include "../usb_msc.h"
#include "../xhci.h"

#define XHCI_MAX_MSC        4
#define XHCI_MSC_RING_TRBS  32
#define XHCI_MSC_MAX_SECT   8

typedef struct xhci_msc {
    usb_msc_dev_t mscdev;
    xhci_controller_t *ctl;
    uint8_t   slot_id;
    uint8_t   port_id;
    uint8_t   speed;
    uint8_t   intf;
    uint8_t   in_ep, out_ep;
    uint8_t   in_dci, out_dci;
    uint16_t  in_mps, out_mps;

    xhci_trb_t *in_ring;
    uintptr_t   in_ring_phys;
    uint16_t    in_enq;
    uint8_t     in_cyc;

    xhci_trb_t *out_ring;
    uintptr_t   out_ring_phys;
    uint16_t    out_enq;
    uint8_t     out_cyc;

    volatile uintptr_t in_pending_trb;
    volatile uintptr_t out_pending_trb;
    volatile uint8_t   in_cc, out_cc;
    volatile uint32_t  in_resid, out_resid;

    struct block_device blk;
    bool      active;
    bool      ready;
    bool      registered;
} xhci_msc_t;

static xhci_msc_t g_msc_devs[XHCI_MAX_MSC];
static int        g_msc_count = 0;

static int xhci_msc_bulk(void *dev, bool dir_in, void *virt, uintptr_t buf_phys,
                         uint32_t len, uint32_t timeout_ms)
{
    (void)virt;
    xhci_msc_t *m = (xhci_msc_t *)dev;
    xhci_trb_t *ring;
    uintptr_t   ring_phys;
    uint16_t   *enq;
    uint8_t    *cyc;
    uint8_t     dci;
    volatile uintptr_t *pending;
    volatile uint8_t   *cc_p;

    if (dir_in) {
        ring = m->in_ring;   ring_phys = m->in_ring_phys;
        enq  = &m->in_enq;   cyc       = &m->in_cyc;
        dci  = m->in_dci;
        pending = &m->in_pending_trb; cc_p = &m->in_cc;
    } else {
        ring = m->out_ring;  ring_phys = m->out_ring_phys;
        enq  = &m->out_enq;  cyc       = &m->out_cyc;
        dci  = m->out_dci;
        pending = &m->out_pending_trb; cc_p = &m->out_cc;
    }

    uintptr_t trb_phys = ring_phys + (uintptr_t)(*enq) * sizeof(xhci_trb_t);
    *pending = trb_phys;
    __asm__ volatile("" ::: "memory");

    xhci_trb_t *t = &ring[*enq];
    t->parameter = (uint64_t)buf_phys;
    t->status    = len & 0xFFFF;
    uint32_t ctl = TRB_TYPE(TRB_NORMAL) | TRB_IOC;
    if (*cyc) ctl |= TRB_CYCLE;
    __asm__ volatile("" ::: "memory");
    t->control = ctl;

    (*enq)++;
    if (*enq == XHCI_MSC_RING_TRBS - 1) {
        xhci_trb_t *link = &ring[XHCI_MSC_RING_TRBS - 1];
        uint32_t lctl = TRB_TYPE(TRB_LINK) | TRB_TC;
        if (*cyc) lctl |= TRB_CYCLE;
        link->control = lctl;
        *enq = 0;
        *cyc ^= 1;
    }

    __asm__ volatile("" ::: "memory");
    m->ctl->doorbells[m->slot_id] = dci;

    uint64_t start = usb_now_ns();
    uint64_t deadline_ns = timeout_ms * 1000000ULL;
    while (*pending != 0) {
        xhci_drain_events(m->ctl);
        if (*pending == 0) break;
        if (usb_now_ns() - start > deadline_ns) {
            serial_printf("[xhci-msc] timeout on bulk %s dci=%u\n",
                          dir_in ? "IN" : "OUT", dci);
            return -EIO;
        }
        cpu_relax();
    }

    uint8_t cc = *cc_p;
    if (cc != CC_SUCCESS && cc != 13) return -EIO;
    return 0;
}

static int msc_blk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    xhci_msc_t *m = (xhci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, buf, false);
}
static int msc_blk_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    xhci_msc_t *m = (xhci_msc_t *)dev->priv;
    if (!m->ready) return -EIO;
    return usb_msc_rw10(&m->mscdev, lba, count, (void *)buf, true);
}
static int msc_blk_flush(struct block_device *dev) {
    xhci_msc_t *m = (xhci_msc_t *)dev->priv;
    if (!m->ready) return 0;
    return usb_msc_sync_cache(&m->mscdev);
}
static struct block_device_ops g_msc_blk_ops = {
    .read  = msc_blk_read,
    .write = msc_blk_write,
    .flush = msc_blk_flush,
};

static int configure_bulk_eps(xhci_controller_t *c, uint8_t slot_id,
                              uint8_t port_id, uint8_t speed,
                              uint8_t in_dci, uint16_t in_mps, uintptr_t in_tr,
                              uint8_t out_dci, uint16_t out_mps, uintptr_t out_tr)
{
    uintptr_t inctx_phys;
    uint8_t *inctx = (uint8_t *)dma_alloc_coherent(4096, &inctx_phys);
    if (!inctx) return -ENOMEM;
    memset(inctx, 0, 4096);

    uint32_t *icc = (uint32_t *)inctx;
    icc[0] = 0;
    icc[1] = (1u << 0) | (1u << in_dci) | (1u << out_dci);

    uint8_t max_dci = (in_dci > out_dci) ? in_dci : out_dci;
    uint32_t *slot_ctx = xhci_input_context(c, inctx, 0);
    slot_ctx[0] = ((uint32_t)max_dci << 27) | ((uint32_t)speed << 20);
    slot_ctx[1] = (uint32_t)port_id << 16;

    uint32_t *ic = xhci_input_context(c, inctx, in_dci);
    ic[0] = 0;
    ic[1] = (3u << 1) | ((uint32_t)EP_TYPE_BULK_IN << 3) | ((uint32_t)in_mps << 16);
    ic[2] = (uint32_t)(in_tr | 1);
    ic[3] = (uint32_t)((uint64_t)in_tr >> 32);
    ic[4] = (uint32_t)in_mps;

    uint32_t *oc = xhci_input_context(c, inctx, out_dci);
    oc[0] = 0;
    oc[1] = (3u << 1) | ((uint32_t)EP_TYPE_BULK_OUT << 3) | ((uint32_t)out_mps << 16);
    oc[2] = (uint32_t)(out_tr | 1);
    oc[3] = (uint32_t)((uint64_t)out_tr >> 32);
    oc[4] = (uint32_t)out_mps;

    xhci_trb_t ev;
    int r = xhci_send_cmd(c, (uint64_t)inctx_phys, 0,
                          TRB_TYPE(TRB_CONFIGURE_ENDPOINT) |
                          ((uint32_t)slot_id << 24),
                          &ev);
    if (r < 0) return r;
    uint8_t cc = (uint8_t)((ev.status >> 24) & 0xFFu);
    if (cc != CC_SUCCESS) {
        serial_printf("[xhci] CONFIGURE_ENDPOINT(MSC): cc=%u\n", cc);
        return -EIO;
    }
    return 0;
}

static int msc_finalize(xhci_msc_t *m, int idx) {
    if (!m->active || m->ready) return 0;

    if (usb_msc_inquiry(&m->mscdev) < 0) {
        serial_printf("[xhci-msc] INQUIRY failed for slot %u\n", m->slot_id);
        return -EIO;
    }
    if (usb_msc_test_unit_ready(&m->mscdev) < 0) {
        serial_printf("[xhci-msc] TEST UNIT READY failed for slot %u\n", m->slot_id);
        return -EIO;
    }
    if (usb_msc_read_capacity(&m->mscdev) < 0) {
        serial_printf("[xhci-msc] READ CAPACITY failed for slot %u\n", m->slot_id);
        return -EIO;
    }

    snprintf(m->blk.name, sizeof(m->blk.name), "usb%d", idx);
    m->blk.sectors     = m->mscdev.lba_count;
    m->blk.sector_size = m->mscdev.block_size;
    m->blk.ops         = &g_msc_blk_ops;
    m->blk.priv        = m;
    m->blk.offset_lba  = 0;
    m->blk.parent      = NULL;
    block_register(&m->blk);
    m->registered = true;
    blockdev_create_node(&m->blk);

    m->ready = true;
    serial_printf("[xhci-msc] /dev/usb%d: %s %s - %llu sectors x %u (%llu MB)\n",
                  idx, m->mscdev.vendor, m->mscdev.product,
                  (unsigned long long)m->mscdev.lba_count, m->mscdev.block_size,
                  (unsigned long long)(m->mscdev.lba_count * (uint64_t)m->mscdev.block_size / (1024 * 1024)));

    partition_rescan_disk(&m->blk);
    return 0;
}

int xhci_msc_register(xhci_controller_t *c, uint8_t slot_id,
                      uint8_t port_id, uint8_t speed,
                      const usb_msc_match_t *match)
{
    if (g_msc_count >= XHCI_MAX_MSC) return -ENOMEM;

    xhci_msc_t *m = &g_msc_devs[g_msc_count];
    memset(m, 0, sizeof(*m));
    m->ctl     = c;
    m->slot_id = slot_id;
    m->port_id = port_id;
    m->speed   = speed;
    m->intf    = match->intf;
    m->in_ep   = match->in_addr  & 0x0F;
    m->out_ep  = match->out_addr & 0x0F;
    m->in_dci  = (uint8_t)(2 * m->in_ep  + 1);
    m->out_dci = (uint8_t)(2 * m->out_ep + 0);
    m->in_mps  = match->in_mps;
    m->out_mps = match->out_mps;

    m->mscdev.ops.dev        = m;
    m->mscdev.ops.bulk       = xhci_msc_bulk;
    m->mscdev.ops.clear_halt = NULL;
    m->mscdev.max_sect       = XHCI_MSC_MAX_SECT;
    m->mscdev.timeout_ms     = 5000;

    m->in_ring = (xhci_trb_t *)dma_alloc_coherent(
        XHCI_MSC_RING_TRBS * sizeof(xhci_trb_t), &m->in_ring_phys);
    if (!m->in_ring) return -ENOMEM;
    memset(m->in_ring, 0, XHCI_MSC_RING_TRBS * sizeof(xhci_trb_t));
    xhci_trb_t *il = &m->in_ring[XHCI_MSC_RING_TRBS - 1];
    il->parameter = (uint64_t)m->in_ring_phys;
    il->control   = TRB_TYPE(TRB_LINK) | TRB_TC;
    m->in_cyc = 1;

    m->out_ring = (xhci_trb_t *)dma_alloc_coherent(
        XHCI_MSC_RING_TRBS * sizeof(xhci_trb_t), &m->out_ring_phys);
    if (!m->out_ring) return -ENOMEM;
    memset(m->out_ring, 0, XHCI_MSC_RING_TRBS * sizeof(xhci_trb_t));
    xhci_trb_t *ol = &m->out_ring[XHCI_MSC_RING_TRBS - 1];
    ol->parameter = (uint64_t)m->out_ring_phys;
    ol->control   = TRB_TYPE(TRB_LINK) | TRB_TC;
    m->out_cyc = 1;

    int r = configure_bulk_eps(c, slot_id, port_id, speed,
                               m->in_dci,  m->in_mps,  m->in_ring_phys,
                               m->out_dci, m->out_mps, m->out_ring_phys);
    if (r < 0) return r;

    m->active = true;
    int idx = g_msc_count++;

    serial_printf("[xhci]   MSC skeleton: slot=%u in_ep=%u(dci=%u,mps=%u) out_ep=%u(dci=%u,mps=%u)\n",
                  slot_id, m->in_ep, m->in_dci, m->in_mps,
                  m->out_ep, m->out_dci, m->out_mps);

    if (c->ready) msc_finalize(m, idx);
    return 0;
}

void xhci_msc_post_init(void) {
    for (int i = 0; i < g_msc_count; i++) {
        msc_finalize(&g_msc_devs[i], i);
    }
}

void xhci_msc_disconnect_slot(uint8_t slot_id) {
    for (int i = 0; i < g_msc_count; i++) {
        if (g_msc_devs[i].active && g_msc_devs[i].slot_id == slot_id) {
            g_msc_devs[i].active = false;
            g_msc_devs[i].ready  = false;

            for (int j = block_count() - 1; j >= 0; j--) {
                struct block_device *c = block_get(j);
                if (c && (c == &g_msc_devs[i].blk || c->parent == &g_msc_devs[i].blk)) {
                    blockdev_remove_node(c);
                    block_unregister(c);
                    if (c != &g_msc_devs[i].blk) kfree(c);
                }
            }
            g_msc_devs[i].registered = false;
            serial_printf("[xhci-hp] /dev/%s removed\n", g_msc_devs[i].blk.name);
        }
    }
}

bool xhci_msc_handle_xfer_event(uint8_t slot_id, uint8_t dci, uint8_t cc,
                                uint32_t resid, uint64_t param)
{
    for (int i = 0; i < g_msc_count; i++) {
        xhci_msc_t *m = &g_msc_devs[i];
        if (!m->active || m->slot_id != slot_id) continue;
        if (dci == m->in_dci) {
            if (m->in_pending_trb && param == m->in_pending_trb) {
                m->in_cc    = cc;
                m->in_resid = resid;
                __asm__ volatile("" ::: "memory");
                m->in_pending_trb = 0;
            }
            return true;
        }
        if (dci == m->out_dci) {
            if (m->out_pending_trb && param == m->out_pending_trb) {
                m->out_cc    = cc;
                m->out_resid = resid;
                __asm__ volatile("" ::: "memory");
                m->out_pending_trb = 0;
            }
            return true;
        }
    }
    return false;
}
