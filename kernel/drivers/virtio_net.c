#include "virtio_net.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/kmemleak.h"
#include "../mm/pmm.h"
#include "../net/net.h"
#include "pci.h"

#define REG_DEVFEAT 0x00u
#define REG_DRVFEAT 0x04u
#define REG_QADDR 0x08u // pfn of virtqueue descriptor table
#define REG_QSIZE 0x0Cu
#define REG_QSEL 0x0Eu
#define REG_QNOTIFY 0x10u
#define REG_STATUS 0x12u
#define REG_ISR 0x13u
#define REG_MAC 0x14u

#define S_ACK 0x01u
#define S_DRIVER 0x02u
#define S_DRVOK 0x04u
#define S_FROK 0x08u
#define S_FAILED 0x80u

#define F_NET_MAC (1u << 5) // device has MAC in config space

#define VQ_F_NEXT 0x1u
#define VQ_F_WRITE 0x2u

#define PCI_VIRTIO_VENDOR 0x1AF4u
#define PCI_VIRTIO_NET_LEG 0x1000u
#define PCI_VIRTIO_NET_MOD 0x1041u /* modern */

#define RX_BUF_SIZE 1536u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256]; /* sized to max supported queue */
} __attribute__((packed)) vq_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vq_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vq_elem_t ring[256];
} __attribute__((packed)) vq_used_t;

typedef struct {
    vq_desc_t *desc;
    vq_avail_t *avail;
    vq_used_t *used;
    uint16_t size;      /* number of descriptors */
    uint16_t next_free; /* next free descriptor slot */
    uint16_t last_used; /* last consumed used-ring index */
} virtq_t;

typedef struct {
    uint8_t flags, gso_type;
    uint16_t hdr_len, gso_size, csum_start, csum_offset;
} __attribute__((packed)) vnet_hdr_t;

static uint16_t g_iobase;
static uint8_t g_mac[6];
static virtq_t g_rxq, g_txq;
static bool g_ready;
static spinlock_irqsave_t g_rxq_lock;
static spinlock_irqsave_t g_txq_lock;

#define RX_PREQUEUE 32u
static uint64_t g_rx_phys[RX_PREQUEUE];

#define TX_SLOTS 32u
static uint64_t g_tx_phys[TX_SLOTS];
static uint16_t g_tx_nslots;

static inline uint8_t io_r8(uint8_t r) { return inb((uint16_t) (g_iobase + r)); }
static inline uint16_t io_r16(uint8_t r) { return inw((uint16_t) (g_iobase + r)); }
static inline uint32_t io_r32(uint8_t r) { return inl((uint16_t) (g_iobase + r)); }
static inline void io_w8(uint8_t r, uint8_t v) { outb((uint16_t) (g_iobase + r), v); }
static inline void io_w16(uint8_t r, uint16_t v) { outw((uint16_t) (g_iobase + r), v); }
static inline void io_w32(uint8_t r, uint32_t v) { outl((uint16_t) (g_iobase + r), v); }
static uint32_t vq_pages(uint16_t size) {
    uint32_t desc_end = (uint32_t) size * 16u;
    uint32_t avail_end = desc_end + 4u + (uint32_t) size * 2u + 2u;
    uint32_t used_off = (avail_end + (uint32_t) (PAGE_SIZE - 1)) & ~(uint32_t) (PAGE_SIZE - 1);
    uint32_t used_end = used_off + 4u + (uint32_t) size * 8u + 2u;
    return (used_end + (uint32_t) (PAGE_SIZE - 1)) / (uint32_t) PAGE_SIZE;
}

static void vq_init(virtq_t *q, uint64_t phys, uint16_t size) {
    uint8_t *v = (uint8_t *) phys_to_virt(phys);
    memset(v, 0, (uint64_t) vq_pages(size) * PAGE_SIZE);

    q->size = size;
    q->next_free = 0;
    q->last_used = 0;
    q->desc = (vq_desc_t *) v;
    q->avail = (vq_avail_t *) (v + (uint32_t) size * 16u);

    uint32_t avail_end = (uint32_t) size * 16u + 4u + (uint32_t) size * 2u + 2u;
    uint32_t used_off = (avail_end + (uint32_t) (PAGE_SIZE - 1)) & ~(uint32_t) (PAGE_SIZE - 1);
    q->used = (vq_used_t *) (v + used_off);
}

static void vq_kick(virtq_t *q, uint16_t desc_idx, uint16_t queue_id) {
    uint16_t ai = q->avail->idx % q->size;
    q->avail->ring[ai] = desc_idx;
    __asm__ volatile("" ::: "memory");
    q->avail->idx++;
    __asm__ volatile("" ::: "memory");
    io_w16(REG_QNOTIFY, queue_id);
}

static void rx_post_buf(uint16_t idx, uint64_t phys) {
    g_rxq.desc[idx].addr = phys;
    g_rxq.desc[idx].len = RX_BUF_SIZE;
    g_rxq.desc[idx].flags = VQ_F_WRITE;
    g_rxq.desc[idx].next = 0;
    vq_kick(&g_rxq, idx, 0);
}

static bool setup_queue(uint16_t qidx, virtq_t *q) {
    io_w16(REG_QSEL, qidx);
    uint16_t sz = io_r16(REG_QSIZE);
    if (!sz) return false;
    if (sz > 256) sz = 256; /* cap to our ring arrays */

    uint32_t n = vq_pages(sz);
    uint64_t phys = (uint64_t) pmm_alloc_contiguous(n);
    if (!phys) return false;

#ifdef CONFIG_KMEMLEAK
    for (uint32_t i = 0; i < n; i++) kmemleak_page_perm((void *) (phys + i * PAGE_SIZE));
#endif

    vq_init(q, phys, sz);
    io_w32(REG_QADDR, (uint32_t) (phys / PAGE_SIZE));
    return true;
}

static void virtnet_irq(int irq, void *arg) {
    (void) irq;
    (void) arg;
    if (!(io_r8(REG_ISR) & 1u)) return;
    virtnet_poll();
}

bool virtnet_ready(void) { return g_ready; }
const uint8_t *virtnet_mac(void) { return g_mac; }

void virtnet_poll(void) {
    if (!g_ready) return;
    spin_lock_irqsave(&g_rxq_lock);
    while (g_rxq.last_used != g_rxq.used->idx) {
        uint16_t ui = g_rxq.last_used % g_rxq.size;
        vq_elem_t e = g_rxq.used->ring[ui];
        g_rxq.last_used++;

        uint16_t di = (uint16_t) e.id;
        if (di >= g_rxq.size) continue; /* device protocol error: never index oob */
        uint8_t *buf = (uint8_t *) phys_to_virt(g_rxq.desc[di].addr);
        uint32_t tot = e.len;
        if (tot > RX_BUF_SIZE) tot = RX_BUF_SIZE; /* clamp to buffer; never overread */

        if (tot > sizeof(vnet_hdr_t))
            net_receive(buf + sizeof(vnet_hdr_t), (uint16_t) (tot - sizeof(vnet_hdr_t)));

        rx_post_buf(di, g_rxq.desc[di].addr); /* repost buffer */
    }
    spin_unlock_irqrestore(&g_rxq_lock);
}

bool virtnet_send(const uint8_t *data, uint16_t len) {
    if (!g_ready || len > 1514u) return false;

    spin_lock_irqsave(&g_txq_lock);
    while (g_txq.last_used != g_txq.used->idx) g_txq.last_used++;

    uint16_t outstanding = (uint16_t) (g_txq.next_free - g_txq.last_used);
    if (outstanding >= g_tx_nslots) {
        spin_unlock_irqrestore(&g_txq_lock);
        return false;
    }

    uint16_t di = (uint16_t) (g_txq.next_free % g_tx_nslots);
    uint8_t *tbuf = (uint8_t *) phys_to_virt(g_tx_phys[di]);
    memset(tbuf, 0, sizeof(vnet_hdr_t));
    memcpy(tbuf + sizeof(vnet_hdr_t), data, len);

    g_txq.desc[di].addr = g_tx_phys[di];
    g_txq.desc[di].len = (uint32_t) (sizeof(vnet_hdr_t) + len);
    g_txq.desc[di].flags = 0;
    g_txq.desc[di].next = 0;

    vq_kick(&g_txq, di, 1);
    g_txq.next_free++;

    spin_unlock_irqrestore(&g_txq_lock);
    return true;
}

void virtnet_init(void) {
    g_ready = false;
    pci_dev_t *dev = NULL;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->vendor == PCI_VIRTIO_VENDOR &&
            (d->device == PCI_VIRTIO_NET_LEG || d->device == PCI_VIRTIO_NET_MOD)) {
            dev = d;
            break;
        }
    }
    if (!dev) {
        log_warn("virtio-net: no PCI device");
        return;
    }

    /* enable bus master ; io decode */
    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->fn, 0x04);
    pci_write32(dev->bus, dev->dev, dev->fn, 0x04, (uint32_t) (cmd | 0x07u));

    /* get io base from bar0 */
    uint32_t bar0 = pci_read32(dev->bus, dev->dev, dev->fn, 0x10);
    if (!(bar0 & 1u)) {
        log_warn("virtio-net: BAR0 not I/O");
        return;
    }
    g_iobase = (uint16_t) (bar0 & ~3u);

    log_info("virtio-net: PCI %02x:%02x.%x iobase=0x%04x irq=%u", dev->bus, dev->dev, dev->fn,
             g_iobase, dev->irq_line);

    io_w8(REG_STATUS, 0);
    io_w8(REG_STATUS, S_ACK);
    io_w8(REG_STATUS, S_ACK | S_DRIVER);

    uint32_t hf = io_r32(REG_DEVFEAT);
    io_w32(REG_DRVFEAT, hf & F_NET_MAC);
    io_w8(REG_STATUS, S_ACK | S_DRIVER | S_FROK);

    for (int i = 0; i < 6; i++) g_mac[i] = io_r8((uint8_t) (REG_MAC + i));
    log_info("virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x", g_mac[0], g_mac[1], g_mac[2],
             g_mac[3], g_mac[4], g_mac[5]);
    if (!setup_queue(0, &g_rxq) || !setup_queue(1, &g_txq)) {
        log_warn("virtio-net: queue setup failed");
        io_w8(REG_STATUS, S_FAILED);
        return;
    }

    /* tx staging pages
             one per usable descriptor slot */
    g_tx_nslots = (TX_SLOTS < g_txq.size) ? (uint16_t) TX_SLOTS : g_txq.size;
    uint16_t got = 0;
    for (uint16_t i = 0; i < g_tx_nslots; i++) {
        void *p = pmm_alloc_zeroed();
        if (!p) break;
        g_tx_phys[i] = (uint64_t) p;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(p);
#endif
        got = (uint16_t) (i + 1);
    }
    g_tx_nslots = got;
    if (!g_tx_nslots) {
        log_warn("virtio-net: no mem for TX bufs");
        io_w8(REG_STATUS, S_FAILED);
        return;
    }

    /* prefill rx queue */
    uint16_t nrx = (RX_PREQUEUE < g_rxq.size) ? RX_PREQUEUE : g_rxq.size;
    for (uint16_t i = 0; i < nrx; i++) {
        void *p = pmm_alloc_zeroed();
        if (!p) break;
        g_rx_phys[i] = (uint64_t) p;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(p);
#endif
        rx_post_buf(i, g_rx_phys[i]);
    }
    io_w8(REG_STATUS, S_ACK | S_DRIVER | S_FROK | S_DRVOK);
    request_irq(dev->irq_line, virtnet_irq, NULL);

    g_ready = true;
    log_info("virtio-net: ready (qsize=%u)", g_rxq.size);
}
