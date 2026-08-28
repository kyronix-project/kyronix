#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/kmemleak.h"
#include "../mm/pmm.h"
#include "../module.h"
#include "../net/net.h"
#include "../bus/pci/pci.h"

#define REG_DEVFEAT 0x00u
#define REG_DRVFEAT 0x04u
#define REG_QADDR 0x08u
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

#define F_NET_MAC (1u << 5)
#define VQ_F_WRITE 0x2u

#define PCI_VIRTIO_VENDOR 0x1AF4u
#define PCI_VIRTIO_NET_LEG 0x1000u
#define PCI_VIRTIO_NET_MOD 0x1041u

#define RX_BUF_SIZE 1536u
#define RX_PREQUEUE 32u
#define TX_SLOTS 32u
#define ENODEV 19
#define ENOMEM 12
#define EBUSY 16

MODULE_NAME("virtio_net");
MODULE_LICENSE("ISC");
MODULE_AUTHOR("Uriy Ovsiannikov");
MODULE_DESCRIPTION("Basic virtio-net PCI driver");

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vq_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
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
    uint64_t phys;
    uint32_t pages;
    uint16_t size;
    uint16_t next_free;
    uint16_t last_used;
} virtq_t;

typedef struct {
    uint8_t flags, gso_type;
    uint16_t hdr_len, gso_size, csum_start, csum_offset;
} __attribute__((packed)) vnet_hdr_t;

static uint16_t g_iobase;
static uint8_t g_mac[6];
static uint8_t g_irq_line;
static bool g_irq_registered;
static bool g_net_registered;
static bool g_ready;
static virtq_t g_rxq, g_txq;
static spinlock_irqsave_t g_rxq_lock;
static spinlock_irqsave_t g_txq_lock;
static uint64_t g_rx_phys[RX_PREQUEUE];
static uint16_t g_rx_nbufs;
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
    uint32_t used_off = (avail_end + (uint32_t) (PAGE_SIZE - 1)) &
                        ~(uint32_t) (PAGE_SIZE - 1);
    uint32_t used_end = used_off + 4u + (uint32_t) size * 8u + 2u;
    return (used_end + (uint32_t) (PAGE_SIZE - 1)) / (uint32_t) PAGE_SIZE;
}

static void vq_init(virtq_t *q, uint64_t phys, uint16_t size, uint32_t pages) {
    uint8_t *v = (uint8_t *) phys_to_virt(phys);
    memset(v, 0, (uint64_t) pages * PAGE_SIZE);
    q->phys = phys;
    q->pages = pages;
    q->size = size;
    q->desc = (vq_desc_t *) v;
    q->avail = (vq_avail_t *) (v + (uint32_t) size * 16u);
    uint32_t avail_end = (uint32_t) size * 16u + 4u + (uint32_t) size * 2u + 2u;
    uint32_t used_off = (avail_end + (uint32_t) (PAGE_SIZE - 1)) &
                        ~(uint32_t) (PAGE_SIZE - 1);
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
    uint16_t size = io_r16(REG_QSIZE);
    if (!size) return false;
    if (size > 256) size = 256;

    uint32_t pages = vq_pages(size);
    uint64_t phys = (uint64_t) pmm_alloc_contiguous(pages);
    if (!phys) return false;
#ifdef CONFIG_KMEMLEAK
    for (uint32_t i = 0; i < pages; i++)
        kmemleak_page_perm((void *) (phys + i * PAGE_SIZE));
#endif
    vq_init(q, phys, size, pages);
    io_w32(REG_QADDR, (uint32_t) (phys / PAGE_SIZE));
    return true;
}

static void virtnet_poll(void);

static void virtnet_irq(int irq, void *arg) {
    (void) irq;
    (void) arg;
    if (!(io_r8(REG_ISR) & 1u)) return;
    net_schedule_poll();
}

static const uint8_t *virtnet_mac(void) { return g_mac; }

static void virtnet_poll(void) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;
    spin_lock_irqsave(&g_rxq_lock);
    while (g_rxq.last_used != g_rxq.used->idx) {
        uint16_t ui = g_rxq.last_used % g_rxq.size;
        vq_elem_t elem = g_rxq.used->ring[ui];
        g_rxq.last_used++;

        uint16_t desc = (uint16_t) elem.id;
        if (desc >= g_rxq.size) continue;
        uint8_t *buf = (uint8_t *) phys_to_virt(g_rxq.desc[desc].addr);
        uint32_t total = elem.len;
        if (total > RX_BUF_SIZE) total = RX_BUF_SIZE;
        if (total > sizeof(vnet_hdr_t))
            net_receive(buf + sizeof(vnet_hdr_t),
                        (uint16_t) (total - sizeof(vnet_hdr_t)));
        rx_post_buf(desc, g_rxq.desc[desc].addr);
    }
    spin_unlock_irqrestore(&g_rxq_lock);
}

static bool virtnet_send(const uint8_t *data, uint16_t len) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || len > 1514u) return false;
    spin_lock_irqsave(&g_txq_lock);
    while (g_txq.last_used != g_txq.used->idx) g_txq.last_used++;

    uint16_t outstanding = (uint16_t) (g_txq.next_free - g_txq.last_used);
    if (outstanding >= g_tx_nslots) {
        spin_unlock_irqrestore(&g_txq_lock);
        return false;
    }

    uint16_t desc = (uint16_t) (g_txq.next_free % g_tx_nslots);
    uint8_t *buf = (uint8_t *) phys_to_virt(g_tx_phys[desc]);
    memset(buf, 0, sizeof(vnet_hdr_t));
    memcpy(buf + sizeof(vnet_hdr_t), data, len);
    g_txq.desc[desc].addr = g_tx_phys[desc];
    g_txq.desc[desc].len = (uint32_t) (sizeof(vnet_hdr_t) + len);
    g_txq.desc[desc].flags = 0;
    g_txq.desc[desc].next = 0;
    vq_kick(&g_txq, desc, 1);
    g_txq.next_free++;
    spin_unlock_irqrestore(&g_txq_lock);
    return true;
}

static const net_driver_ops_t g_net_ops = {
    .send = virtnet_send,
    .poll = virtnet_poll,
    .mac = virtnet_mac,
};

static void release_queue(virtq_t *queue) {
    if (queue->phys) pmm_free_contiguous((void *) queue->phys, queue->pages);
    memset(queue, 0, sizeof(*queue));
}

static void virtnet_stop(void) {
    __atomic_store_n(&g_ready, false, __ATOMIC_RELEASE);
    if (g_irq_registered) {
        free_irq(g_irq_line, virtnet_irq, NULL);
        g_irq_registered = false;
    }
    if (g_net_registered) {
        net_driver_unregister(&g_net_ops);
        g_net_registered = false;
    }

    spin_lock_irqsave(&g_rxq_lock);
    spin_unlock_irqrestore(&g_rxq_lock);
    spin_lock_irqsave(&g_txq_lock);
    spin_unlock_irqrestore(&g_txq_lock);
    if (g_iobase) io_w8(REG_STATUS, 0);

    for (uint16_t i = 0; i < g_rx_nbufs; i++) {
        pmm_free((void *) g_rx_phys[i]);
        g_rx_phys[i] = 0;
    }
    g_rx_nbufs = 0;
    for (uint16_t i = 0; i < g_tx_nslots; i++) {
        pmm_free((void *) g_tx_phys[i]);
        g_tx_phys[i] = 0;
    }
    g_tx_nslots = 0;
    release_queue(&g_rxq);
    release_queue(&g_txq);
    g_iobase = 0;
}

static int virtnet_start(void) {
    pci_dev_t *dev = NULL;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *candidate = &g_pci_devs[i];
        if (candidate->vendor == PCI_VIRTIO_VENDOR &&
            (candidate->device == PCI_VIRTIO_NET_LEG ||
             candidate->device == PCI_VIRTIO_NET_MOD)) {
            dev = candidate;
            break;
        }
    }
    if (!dev) {
        log_warn("virtio-net: no PCI device");
        return -ENODEV;
    }

    uint16_t command = pci_read16(dev->bus, dev->dev, dev->fn, 0x04);
    pci_write32(dev->bus, dev->dev, dev->fn, 0x04, (uint32_t) (command | 0x07u));
    uint32_t bar0 = pci_read32(dev->bus, dev->dev, dev->fn, 0x10);
    if (!(bar0 & 1u)) {
        log_warn("virtio-net: BAR0 not I/O");
        return -ENODEV;
    }
    g_iobase = (uint16_t) (bar0 & ~3u);
    g_irq_line = dev->irq_line;
    if (g_irq_line >= 16) {
        g_iobase = 0;
        log_warn("virtio-net: unsupported IRQ %u", dev->irq_line);
        return -ENODEV;
    }

    log_info("virtio-net: PCI %02x:%02x.%x iobase=0x%04x irq=%u", dev->bus, dev->dev,
             dev->fn, g_iobase, dev->irq_line);
    io_w8(REG_STATUS, 0);
    io_w8(REG_STATUS, S_ACK);
    io_w8(REG_STATUS, S_ACK | S_DRIVER);
    uint32_t host_features = io_r32(REG_DEVFEAT);
    io_w32(REG_DRVFEAT, host_features & F_NET_MAC);
    io_w8(REG_STATUS, S_ACK | S_DRIVER | S_FROK);

    for (int i = 0; i < 6; i++) g_mac[i] = io_r8((uint8_t) (REG_MAC + i));
    if (!setup_queue(0, &g_rxq) || !setup_queue(1, &g_txq)) {
        log_warn("virtio-net: queue setup failed");
        virtnet_stop();
        return -ENOMEM;
    }

    uint16_t tx_slots = TX_SLOTS < g_txq.size ? TX_SLOTS : g_txq.size;
    for (uint16_t i = 0; i < tx_slots; i++) {
        void *page = pmm_alloc_zeroed();
        if (!page) break;
        g_tx_phys[g_tx_nslots++] = (uint64_t) page;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(page);
#endif
    }
    if (!g_tx_nslots) {
        virtnet_stop();
        return -ENOMEM;
    }

    uint16_t rx_slots = RX_PREQUEUE < g_rxq.size ? RX_PREQUEUE : g_rxq.size;
    for (uint16_t i = 0; i < rx_slots; i++) {
        void *page = pmm_alloc_zeroed();
        if (!page) break;
        g_rx_phys[g_rx_nbufs++] = (uint64_t) page;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(page);
#endif
        rx_post_buf(i, (uint64_t) page);
    }
    if (!g_rx_nbufs) {
        virtnet_stop();
        return -ENOMEM;
    }

    io_w8(REG_STATUS, S_ACK | S_DRIVER | S_FROK | S_DRVOK);
    request_irq(g_irq_line, virtnet_irq, NULL);
    g_irq_registered = true;
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    if (!net_driver_register(&g_net_ops)) {
        virtnet_stop();
        return -EBUSY;
    }
    g_net_registered = true;
    log_info("virtio-net: ready (qsize=%u)", g_rxq.size);
    return 0;
}

module_init(virtnet_start);
module_exit(virtnet_stop);
