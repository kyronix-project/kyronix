#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../module.h"
#include "../net/net.h"
#include "../bus/pci/pci.h"

MODULE_NAME("rtl8168");
MODULE_LICENSE("GPL-2.0");
MODULE_AUTHOR("Kyronix Authors");
MODULE_DESCRIPTION("Realtek RTL8111/8168 PCIe Ethernet Driver");

#define RTL_VENDOR 0x10ECu
#define RTL_DEV_8168 0x8168u
#define RTL_DEV_8169 0x8169u
#define RTL_DEV_8167 0x8167u
#define RTL_DEV_8136 0x8136u

#define RTL_MMIO_VBASE 0xffff960000000000ULL
#define RTL_DESC_COUNT 64u
#define RTL_BUF_SIZE 2048u
#define RTL_MAX_FRAME 1514u

#define REG_MAC0       0x00
#define REG_MAR0       0x08
#define REG_TNPDS      0x20
#define REG_TNPDS_HI   0x24
#define REG_COMMAND    0x37
#define REG_TX_POLL    0x38
#define REG_INTR_MASK  0x3C
#define REG_INTR_STAT  0x3E
#define REG_TX_CONFIG  0x40
#define REG_RX_CONFIG  0x44
#define REG_CFG9346    0x50
#define REG_RX_MAX     0xDA
#define REG_CPLUS_CMD  0xE0
#define REG_RX_DESC    0xE4
#define REG_RX_DESC_HI 0xE8

#define CMD_RESET 0x10
#define CMD_RX_ENABLE 0x08
#define CMD_TX_ENABLE 0x04
#define DESC_OWN (1u << 31)
#define DESC_EOR (1u << 30)
#define DESC_FS  (1u << 29)
#define DESC_LS  (1u << 28)
#define DESC_ERR (1u << 21)
#define INTR_RX_OK 0x0001
#define INTR_RX_ERR 0x0002
#define INTR_TX_OK 0x0004
#define INTR_LINK 0x0020

typedef struct __attribute__((packed, aligned(16))) rtl_desc {
    uint32_t opts1;
    uint32_t opts2;
    uint64_t addr;
} rtl_desc_t;

static spinlock_irqsave_t g_rx_lock;
static spinlock_irqsave_t g_tx_lock;
static uint64_t g_mmio_pages;
static uint64_t g_rx_ring_phys;
static uint64_t g_tx_ring_phys;
static rtl_desc_t *g_rx_ring;
static rtl_desc_t *g_tx_ring;
static void *g_rx_pages[RTL_DESC_COUNT];
static void *g_tx_pages[RTL_DESC_COUNT];
static uint8_t g_mac[6];
static uint16_t g_rx_cur;
static uint16_t g_tx_cur;
static uint8_t g_irq;
static bool g_irq_registered;
static bool g_net_registered;
static bool g_hw_touched;
static volatile bool g_ready;

static inline volatile uint8_t *rtl_reg(uint32_t reg) {
    return (volatile uint8_t *)(RTL_MMIO_VBASE + reg);
}

static inline uint8_t rtl_r8(uint32_t reg) { return *rtl_reg(reg); }
static inline uint16_t rtl_r16(uint32_t reg) { return *(volatile uint16_t *)rtl_reg(reg); }
static inline uint32_t rtl_r32(uint32_t reg) { return *(volatile uint32_t *)rtl_reg(reg); }
static inline void rtl_w8(uint32_t reg, uint8_t val) { *rtl_reg(reg) = val; }
static inline void rtl_w16(uint32_t reg, uint16_t val) { *(volatile uint16_t *)rtl_reg(reg) = val; }
static inline void rtl_w32(uint32_t reg, uint32_t val) { *(volatile uint32_t *)rtl_reg(reg) = val; }

static bool rtl_supported(uint16_t id) {
    return id == RTL_DEV_8168 || id == RTL_DEV_8169 || id == RTL_DEV_8167 || id == RTL_DEV_8136;
}

static const uint8_t *rtl_mac(void) { return g_mac; }

static bool rtl_send(const uint8_t *data, uint16_t len) {
    if (!g_ready || !data || len == 0 || len > RTL_MAX_FRAME) return false;
    spin_lock_irqsave(&g_tx_lock);
    uint16_t idx = g_tx_cur;
    rtl_desc_t *d = &g_tx_ring[idx];
    if (d->opts1 & DESC_OWN) {
        spin_unlock_irqrestore(&g_tx_lock);
        return false;
    }
    memcpy(phys_to_virt((uint64_t)g_tx_pages[idx]), data, len);
    d->opts2 = 0;
    d->opts1 = DESC_FS | DESC_LS | len | (idx == RTL_DESC_COUNT - 1 ? DESC_EOR : 0);
    __asm__ volatile("sfence" ::: "memory");
    d->opts1 |= DESC_OWN;
    g_tx_cur = (idx + 1) % RTL_DESC_COUNT;
    rtl_w8(REG_TX_POLL, 0x40);
    spin_unlock_irqrestore(&g_tx_lock);
    return true;
}

static void rtl_poll(void) {
    if (!g_ready) return;
    spin_lock_irqsave(&g_rx_lock);
    for (unsigned n = 0; n < RTL_DESC_COUNT; n++) {
        uint16_t idx = g_rx_cur;
        rtl_desc_t *d = &g_rx_ring[idx];
        __asm__ volatile("lfence" ::: "memory");
        uint32_t status = d->opts1;
        if (status & DESC_OWN) break;
        uint32_t len = status & 0x3FFFu;
        if (!(status & DESC_ERR) && len > 4 && len <= RTL_BUF_SIZE) {
            net_receive((const uint8_t *)phys_to_virt((uint64_t)g_rx_pages[idx]),
                        (uint16_t)(len - 4));
        }
        d->opts2 = 0;
        d->opts1 = DESC_OWN | RTL_BUF_SIZE |
                   (idx == RTL_DESC_COUNT - 1 ? DESC_EOR : 0);
        g_rx_cur = (idx + 1) % RTL_DESC_COUNT;
    }
    spin_unlock_irqrestore(&g_rx_lock);
}

static void rtl_irq(int irq, void *arg) {
    (void)irq;
    (void)arg;
    uint16_t status = rtl_r16(REG_INTR_STAT);
    if (!status || status == 0xFFFF) return;
    rtl_w16(REG_INTR_STAT, status);
    if (status & (INTR_RX_OK | INTR_RX_ERR | INTR_TX_OK | INTR_LINK)) net_schedule_poll();
}

static void rtl_stop(void) {
    __atomic_store_n(&g_ready, false, __ATOMIC_RELEASE);
    if (g_hw_touched) {
        rtl_w16(REG_INTR_MASK, 0);
        rtl_w8(REG_COMMAND, 0);
        g_hw_touched = false;
    }
    if (g_irq_registered) {
        free_irq(g_irq, rtl_irq, NULL);
        g_irq_registered = false;
    }
    if (g_net_registered) {
        extern const net_driver_ops_t g_rtl_net_ops;
        net_driver_unregister(&g_rtl_net_ops);
        g_net_registered = false;
    }
    for (unsigned i = 0; i < RTL_DESC_COUNT; i++) {
        if (g_rx_pages[i]) { pmm_free(g_rx_pages[i]); g_rx_pages[i] = NULL; }
        if (g_tx_pages[i]) { pmm_free(g_tx_pages[i]); g_tx_pages[i] = NULL; }
    }
    if (g_rx_ring_phys) { pmm_free((void *)g_rx_ring_phys); g_rx_ring_phys = 0; }
    if (g_tx_ring_phys) { pmm_free((void *)g_tx_ring_phys); g_tx_ring_phys = 0; }
    g_rx_ring = NULL;
    g_tx_ring = NULL;
    for (uint64_t i = 0; i < g_mmio_pages; i++)
        vmm_unmap(&g_kernel_space, RTL_MMIO_VBASE + i * PAGE_SIZE);
    g_mmio_pages = 0;
}

const net_driver_ops_t g_rtl_net_ops = {
    .send = rtl_send,
    .poll = rtl_poll,
    .mac = rtl_mac,
};

static int rtl_start(void) {
    pci_dev_t *dev = NULL;
    uint64_t mmio_base = 0;
    uint32_t mmio_size = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        if (g_pci_devs[i].vendor == RTL_VENDOR && rtl_supported(g_pci_devs[i].device)) {
            dev = &g_pci_devs[i];
            break;
        }
    }
    if (!dev) return -19;

    for (unsigned i = 0; i < 6; i++) {
        if (dev->bars[i]) {
            mmio_base = dev->bars[i];
            mmio_size = dev->bar_sizes[i];
            break;
        }
    }
    if (!mmio_base) return -19;

    uint64_t size = mmio_size ? mmio_size : PAGE_SIZE;
    uint64_t pages_needed = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (!pages_needed || pages_needed > 16) return -19;
    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->fn, 0x04);
    pci_write16(dev->bus, dev->dev, dev->fn, 0x04, cmd | 0x0006u);
    for (uint64_t i = 0; i < pages_needed; i++) {
        if (vmm_map(&g_kernel_space, RTL_MMIO_VBASE + i * PAGE_SIZE,
                    mmio_base + i * PAGE_SIZE,
                    VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_PCD) < 0) {
            rtl_stop();
            return -12;
        }
        g_mmio_pages++;
    }

    g_hw_touched = true;
    rtl_w16(REG_INTR_MASK, 0);
    rtl_w8(REG_COMMAND, CMD_RESET);
    unsigned timeout = 1000000;
    while ((rtl_r8(REG_COMMAND) & CMD_RESET) && timeout--) cpu_relax();
    if (!timeout) { rtl_stop(); return -19; }

    for (unsigned i = 0; i < 6; i++) g_mac[i] = rtl_r8(REG_MAC0 + i);
    bool mac_ok = false;
    for (unsigned i = 0; i < 6; i++) if (g_mac[i] && g_mac[i] != 0xFF) mac_ok = true;
    if (!mac_ok) { rtl_stop(); return -19; }

    void *rx_ring = pmm_alloc_zeroed();
    void *tx_ring = pmm_alloc_zeroed();
    if (!rx_ring || !tx_ring) {
        if (rx_ring) pmm_free(rx_ring);
        if (tx_ring) pmm_free(tx_ring);
        rtl_stop();
        return -12;
    }
    g_rx_ring_phys = (uint64_t)rx_ring;
    g_tx_ring_phys = (uint64_t)tx_ring;
    g_rx_ring = (rtl_desc_t *)phys_to_virt(g_rx_ring_phys);
    g_tx_ring = (rtl_desc_t *)phys_to_virt(g_tx_ring_phys);

    for (unsigned i = 0; i < RTL_DESC_COUNT; i++) {
        g_rx_pages[i] = pmm_alloc_zeroed();
        g_tx_pages[i] = pmm_alloc_zeroed();
        if (!g_rx_pages[i] || !g_tx_pages[i]) { rtl_stop(); return -12; }
        g_rx_ring[i].addr = (uint64_t)g_rx_pages[i];
        g_rx_ring[i].opts2 = 0;
        g_rx_ring[i].opts1 = DESC_OWN | RTL_BUF_SIZE |
                             (i == RTL_DESC_COUNT - 1 ? DESC_EOR : 0);
        g_tx_ring[i].addr = (uint64_t)g_tx_pages[i];
        g_tx_ring[i].opts2 = 0;
        g_tx_ring[i].opts1 = i == RTL_DESC_COUNT - 1 ? DESC_EOR : 0;
    }

    rtl_w32(REG_TNPDS, (uint32_t)g_tx_ring_phys);
    rtl_w32(REG_TNPDS_HI, (uint32_t)(g_tx_ring_phys >> 32));
    rtl_w32(REG_RX_DESC, (uint32_t)g_rx_ring_phys);
    rtl_w32(REG_RX_DESC_HI, (uint32_t)(g_rx_ring_phys >> 32));
    rtl_w16(REG_RX_MAX, RTL_BUF_SIZE);
    rtl_w16(REG_CPLUS_CMD, rtl_r16(REG_CPLUS_CMD) | 0x0001u);
    rtl_w32(REG_TX_CONFIG, 0x03000700u);
    rtl_w32(REG_RX_CONFIG, 0x0000E71Du);
    rtl_w32(REG_MAR0, 0xFFFFFFFFu);
    rtl_w32(REG_MAR0 + 4, 0xFFFFFFFFu);
    rtl_w16(REG_INTR_STAT, 0xFFFFu);

    g_irq = dev->irq_line;
    if (g_irq < 16) {
        request_irq(g_irq, rtl_irq, NULL);
        g_irq_registered = true;
        rtl_w16(REG_INTR_MASK, INTR_RX_OK | INTR_RX_ERR | INTR_TX_OK | INTR_LINK);
    }
    __asm__ volatile("sfence" ::: "memory");
    rtl_w8(REG_COMMAND, CMD_RX_ENABLE | CMD_TX_ENABLE);
    g_rx_cur = 0;
    g_tx_cur = 0;
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);
    if (!net_driver_register(&g_rtl_net_ops)) { rtl_stop(); return -16; }
    g_net_registered = true;
    net_schedule_poll();
    return 0;
}

static void rtl_exit(void) { rtl_stop(); }

module_init(rtl_start);
module_exit(rtl_exit);
