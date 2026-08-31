#include "e1000.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../mm/kmemleak.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../module.h"
#include "../net/net.h"
#include "../bus/pci/pci.h"

MODULE_NAME("e1000");
MODULE_LICENSE("GPL-2.0");
MODULE_AUTHOR("Kyronix Authors");
MODULE_DESCRIPTION("Intel PRO/1000 Gigabit Ethernet Driver");

#define E1000_MMIO_VBASE 0xffff940000000000ULL
#define ENODEV 19
#define ENOMEM 12
#define EBUSY 16

static spinlock_irqsave_t g_rx_lock;
static spinlock_irqsave_t g_tx_lock;

static uint64_t g_mmio_vbase = E1000_MMIO_VBASE;
static uint32_t g_mmio_pages = 0;
static uint8_t g_irq_line = 0;
static bool g_irq_registered = false;
static bool g_net_registered = false;
static volatile bool g_ready = false;
static uint8_t g_mac[6];

static uint64_t g_rx_ring_phys = 0;
static e1000_rx_desc_t *g_rx_descs = NULL;
static void *g_rx_pages[E1000_NUM_RX_DESC / 2];
static uint8_t *g_rx_buf_virt[E1000_NUM_RX_DESC];
static uint16_t g_rx_cur = 0;

static uint64_t g_tx_ring_phys = 0;
static e1000_tx_desc_t *g_tx_descs = NULL;
static void *g_tx_pages[E1000_NUM_TX_DESC / 2];
static uint8_t *g_tx_buf_virt[E1000_NUM_TX_DESC];
static uint16_t g_tx_cur = 0;

static const uint16_t g_supported_dev_ids[] = {
    E1000_DEV_ID_82542,
    E1000_DEV_ID_82542_FIBER,
    E1000_DEV_ID_82543GC_FIBER,
    E1000_DEV_ID_82543GC_COPPER,
    E1000_DEV_ID_82544EI_COPPER,
    E1000_DEV_ID_82544EI_FIBER,
    E1000_DEV_ID_82544GC_COPPER,
    E1000_DEV_ID_82544GC_LOM,
    E1000_DEV_ID_82540EM,
    E1000_DEV_ID_82540EM_LOM,
    E1000_DEV_ID_82540EP_LOM,
    E1000_DEV_ID_82540EP,
    E1000_DEV_ID_82540EP_LP,
    E1000_DEV_ID_82545EM_COPPER,
    E1000_DEV_ID_82545EM_FIBER,
    E1000_DEV_ID_82545GM_COPPER,
    E1000_DEV_ID_82545GM_FIBER,
    E1000_DEV_ID_82545GM_SERDES,
    E1000_DEV_ID_82546EB_COPPER,
    E1000_DEV_ID_82546EB_FIBER,
    E1000_DEV_ID_82546EB_QUAD_COPPER,
    E1000_DEV_ID_82546GB_COPPER,
    E1000_DEV_ID_82546GB_FIBER,
    E1000_DEV_ID_82546GB_SERDES,
    E1000_DEV_ID_82546GB_PCIE,
    E1000_DEV_ID_82546GB_QUAD_COPPER,
    E1000_DEV_ID_82546GB_QUAD_COPPER_KSP3,
    E1000_DEV_ID_82541EI,
    E1000_DEV_ID_82541EI_MOBILE,
    E1000_DEV_ID_82541ER_LOM,
    E1000_DEV_ID_82541ER,
    E1000_DEV_ID_82541GI,
    E1000_DEV_ID_82541GI_MOBILE,
    E1000_DEV_ID_82541GI_LF,
    E1000_DEV_ID_82547EI,
    E1000_DEV_ID_82547EI_MOBILE,
    E1000_DEV_ID_82547GI
};

static inline void e1000_write32(uint32_t reg, uint32_t val) {
    volatile uint32_t *addr = (volatile uint32_t *) (g_mmio_vbase + reg);
    *addr = val;
}

static inline uint32_t e1000_read32(uint32_t reg) {
    volatile uint32_t *addr = (volatile uint32_t *) (g_mmio_vbase + reg);
    return *addr;
}

static bool e1000_is_supported_device(uint16_t dev_id) {
    for (size_t i = 0; i < sizeof(g_supported_dev_ids) / sizeof(g_supported_dev_ids[0]); i++) {
        if (g_supported_dev_ids[i] == dev_id) {
            return true;
        }
    }
    return false;
}

static void e1000_read_mac(void) {
    bool eerd_valid = false;
    for (uint32_t i = 0; i < 3; i++) {
        e1000_write32(E1000_REG_EERD, E1000_EERD_START | (i << E1000_EERD_ADDR_SHIFT));
        uint32_t val = 0;
        int timeout = 20000;
        while (timeout-- > 0) {
            val = e1000_read32(E1000_REG_EERD);
            if (val & E1000_EERD_DONE) {
                eerd_valid = true;
                break;
            }
            io_wait();
        }
        if (val & E1000_EERD_DONE) {
            uint16_t data = (uint16_t) ((val >> E1000_EERD_DATA_SHIFT) & 0xFFFF);
            g_mac[i * 2] = (uint8_t) (data & 0xFF);
            g_mac[i * 2 + 1] = (uint8_t) ((data >> 8) & 0xFF);
        }
    }

    if (!eerd_valid ||
        (g_mac[0] == 0 && g_mac[1] == 0 && g_mac[2] == 0 && g_mac[3] == 0 && g_mac[4] == 0 && g_mac[5] == 0) ||
        (g_mac[0] == 0xFF && g_mac[1] == 0xFF && g_mac[2] == 0xFF && g_mac[3] == 0xFF && g_mac[4] == 0xFF && g_mac[5] == 0xFF)) {
        uint32_t ral = e1000_read32(E1000_REG_RAL);
        uint32_t rah = e1000_read32(E1000_REG_RAH);
        g_mac[0] = (uint8_t) (ral & 0xFF);
        g_mac[1] = (uint8_t) ((ral >> 8) & 0xFF);
        g_mac[2] = (uint8_t) ((ral >> 16) & 0xFF);
        g_mac[3] = (uint8_t) ((ral >> 24) & 0xFF);
        g_mac[4] = (uint8_t) (rah & 0xFF);
        g_mac[5] = (uint8_t) ((rah >> 8) & 0xFF);
    }

    uint32_t ral = (uint32_t) g_mac[0] | ((uint32_t) g_mac[1] << 8) |
                   ((uint32_t) g_mac[2] << 16) | ((uint32_t) g_mac[3] << 24);
    uint32_t rah = (uint32_t) g_mac[4] | ((uint32_t) g_mac[5] << 8) | E1000_RAH_AV;
    e1000_write32(E1000_REG_RAL, ral);
    e1000_write32(E1000_REG_RAH, rah);
}

static const uint8_t *e1000_mac(void) {
    return g_mac;
}

static void e1000_poll(void) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) return;

    spin_lock_irqsave(&g_rx_lock);
    int count = 0;
    uint16_t last_processed = g_rx_cur;

    while (g_rx_descs[g_rx_cur].status & E1000_RXD_STAT_DD) {
        uint8_t status = g_rx_descs[g_rx_cur].status;
        uint8_t errors = g_rx_descs[g_rx_cur].errors;
        uint16_t len = g_rx_descs[g_rx_cur].length;

        if ((status & E1000_RXD_STAT_EOP) &&
            !(errors & (E1000_RXD_ERR_CE | E1000_RXD_ERR_SE | E1000_RXD_ERR_SEQ | E1000_RXD_ERR_RXE)) &&
            len > 0 && len <= E1000_RX_BUFFER_SIZE) {
            uint8_t *pkt = g_rx_buf_virt[g_rx_cur];
            net_receive(pkt, len);
        }

        g_rx_descs[g_rx_cur].status = 0;
        g_rx_descs[g_rx_cur].errors = 0;
        g_rx_descs[g_rx_cur].length = 0;
        last_processed = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % E1000_NUM_RX_DESC;
        count++;
    }

    if (count > 0) {
        e1000_write32(E1000_REG_RDT, last_processed);
    }
    spin_unlock_irqrestore(&g_rx_lock);
}

static bool e1000_send(const uint8_t *data, uint16_t len) {
    if (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE) || !data || len == 0 || len > 1514u) {
        return false;
    }

    spin_lock_irqsave(&g_tx_lock);

    if (!(g_tx_descs[g_tx_cur].status & E1000_TXD_STAT_DD)) {
        spin_unlock_irqrestore(&g_tx_lock);
        return false;
    }

    uint8_t *tx_buf = g_tx_buf_virt[g_tx_cur];
    memcpy(tx_buf, data, len);

    g_tx_descs[g_tx_cur].length = len;
    g_tx_descs[g_tx_cur].cso = 0;
    g_tx_descs[g_tx_cur].cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
    g_tx_descs[g_tx_cur].status = 0;
    g_tx_descs[g_tx_cur].css = 0;
    g_tx_descs[g_tx_cur].special = 0;

    __asm__ volatile("" ::: "memory");

    uint16_t next_tx = (g_tx_cur + 1) % E1000_NUM_TX_DESC;
    g_tx_cur = next_tx;
    e1000_write32(E1000_REG_TDT, next_tx);

    spin_unlock_irqrestore(&g_tx_lock);
    return true;
}

static void e1000_irq(int irq, void *arg) {
    (void) irq;
    (void) arg;

    uint32_t icr = e1000_read32(E1000_REG_ICR);
    if (icr == 0 || icr == 0xFFFFFFFF) {
        return;
    }

    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXO | E1000_ICR_RXDMT0 | E1000_ICR_RXSEQ | E1000_ICR_TXDW | E1000_ICR_TXQE)) {
        net_schedule_poll();
    }

    if (icr & E1000_ICR_LSC) {
        uint32_t status = e1000_read32(E1000_REG_STATUS);
        (void) status;
    }
}

static const net_driver_ops_t g_e1000_net_ops = {
    .send = e1000_send,
    .poll = e1000_poll,
    .mac = e1000_mac,
};

static void e1000_stop(void) {
    __atomic_store_n(&g_ready, false, __ATOMIC_RELEASE);

    if (g_irq_registered) {
        free_irq(g_irq_line, e1000_irq, NULL);
        g_irq_registered = false;
    }

    if (g_net_registered) {
        net_driver_unregister(&g_e1000_net_ops);
        g_net_registered = false;
    }

    spin_lock_irqsave(&g_rx_lock);
    spin_unlock_irqrestore(&g_rx_lock);
    spin_lock_irqsave(&g_tx_lock);
    spin_unlock_irqrestore(&g_tx_lock);

    if (g_mmio_pages > 0) {
        e1000_write32(E1000_REG_IMC, 0xFFFFFFFF);
        (void) e1000_read32(E1000_REG_ICR);
        e1000_write32(E1000_REG_RCTL, 0);
        e1000_write32(E1000_REG_TCTL, 0);

        for (uint32_t i = 0; i < g_mmio_pages; i++) {
            vmm_unmap(&g_kernel_space, g_mmio_vbase + (uint64_t) i * PAGE_SIZE);
        }
        g_mmio_pages = 0;
    }

    if (g_rx_ring_phys) {
        pmm_free((void *) g_rx_ring_phys);
        g_rx_ring_phys = 0;
    }
    for (size_t i = 0; i < sizeof(g_rx_pages) / sizeof(g_rx_pages[0]); i++) {
        if (g_rx_pages[i]) {
            pmm_free(g_rx_pages[i]);
            g_rx_pages[i] = NULL;
        }
    }
    if (g_tx_ring_phys) {
        pmm_free((void *) g_tx_ring_phys);
        g_tx_ring_phys = 0;
    }
    for (size_t i = 0; i < sizeof(g_tx_pages) / sizeof(g_tx_pages[0]); i++) {
        if (g_tx_pages[i]) {
            pmm_free(g_tx_pages[i]);
            g_tx_pages[i] = NULL;
        }
    }
}

static int e1000_init_hw(const pci_dev_t *dev) {
    uint64_t pbase = dev->bars[0];
    if (!pbase) return -ENODEV;

    g_mmio_pages = (dev->bar_sizes[0] + PAGE_SIZE - 1) / PAGE_SIZE;
    if (g_mmio_pages == 0) g_mmio_pages = 32;

    for (uint32_t i = 0; i < g_mmio_pages; i++) {
        int ret = vmm_map(&g_kernel_space, g_mmio_vbase + (uint64_t) i * PAGE_SIZE,
                          pbase + (uint64_t) i * PAGE_SIZE,
                          VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_PCD);
        if (ret < 0) {
            for (uint32_t j = 0; j < i; j++) {
                vmm_unmap(&g_kernel_space, g_mmio_vbase + (uint64_t) j * PAGE_SIZE);
            }
            g_mmio_pages = 0;
            return ret;
        }
    }

    uint16_t cmd = pci_read16(dev->bus, dev->dev, dev->fn, 0x04);
    pci_write16(dev->bus, dev->dev, dev->fn, 0x04, (cmd | 0x0006u));

    e1000_write32(E1000_REG_IMC, 0xFFFFFFFF);
    (void) e1000_read32(E1000_REG_ICR);

    e1000_write32(E1000_REG_CTRL, E1000_CTRL_RST);
    for (int i = 0; i < 50000; i++) io_wait();

    e1000_write32(E1000_REG_IMC, 0xFFFFFFFF);
    (void) e1000_read32(E1000_REG_ICR);

    e1000_read_mac();

    e1000_write32(E1000_REG_CTRL, E1000_CTRL_SLU | E1000_CTRL_ASDE | E1000_CTRL_FD);

    int link_timeout = 100000;
    while (link_timeout-- > 0) {
        if (e1000_read32(E1000_REG_STATUS) & E1000_STATUS_LU) break;
        io_wait();
    }

    e1000_write32(E1000_REG_FCAL, 0x00C28001);
    e1000_write32(E1000_REG_FCAH, 0x0100);
    e1000_write32(E1000_REG_FCT, 0x8808);
    e1000_write32(E1000_REG_FCTTV, 0x0100);

    for (int i = 0; i < 128; i++) {
        e1000_write32(E1000_REG_MTA + i * 4, 0);
    }

    void *rx_ring_page = pmm_alloc_zeroed();
    if (!rx_ring_page) return -ENOMEM;
    g_rx_ring_phys = (uint64_t) rx_ring_page;
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm(rx_ring_page);
#endif
    g_rx_descs = (e1000_rx_desc_t *) phys_to_virt(g_rx_ring_phys);

    for (int i = 0; i < E1000_NUM_RX_DESC / 2; i++) {
        void *p = pmm_alloc_zeroed();
        if (!p) return -ENOMEM;
        g_rx_pages[i] = p;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(p);
#endif
        uint64_t p_phys = (uint64_t) p;
        uint8_t *p_virt = (uint8_t *) phys_to_virt(p_phys);

        g_rx_descs[i * 2].buffer_addr = p_phys;
        g_rx_descs[i * 2].status = 0;
        g_rx_buf_virt[i * 2] = p_virt;

        g_rx_descs[i * 2 + 1].buffer_addr = p_phys + E1000_RX_BUFFER_SIZE;
        g_rx_descs[i * 2 + 1].status = 0;
        g_rx_buf_virt[i * 2 + 1] = p_virt + E1000_RX_BUFFER_SIZE;
    }

    g_rx_cur = 0;
    e1000_write32(E1000_REG_RDBAL, (uint32_t) (g_rx_ring_phys & 0xFFFFFFFF));
    e1000_write32(E1000_REG_RDBAH, (uint32_t) (g_rx_ring_phys >> 32));
    e1000_write32(E1000_REG_RDLEN, E1000_NUM_RX_DESC * sizeof(e1000_rx_desc_t));
    e1000_write32(E1000_REG_RDH, 0);
    e1000_write32(E1000_REG_RDT, E1000_NUM_RX_DESC - 1);
    e1000_write32(E1000_REG_RDTR, 0);
    e1000_write32(E1000_REG_RADV, 0);
    e1000_write32(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC);

    void *tx_ring_page = pmm_alloc_zeroed();
    if (!tx_ring_page) return -ENOMEM;
    g_tx_ring_phys = (uint64_t) tx_ring_page;
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm(tx_ring_page);
#endif
    g_tx_descs = (e1000_tx_desc_t *) phys_to_virt(g_tx_ring_phys);

    for (int i = 0; i < E1000_NUM_TX_DESC / 2; i++) {
        void *p = pmm_alloc_zeroed();
        if (!p) return -ENOMEM;
        g_tx_pages[i] = p;
#ifdef CONFIG_KMEMLEAK
        kmemleak_page_perm(p);
#endif
        uint64_t p_phys = (uint64_t) p;
        uint8_t *p_virt = (uint8_t *) phys_to_virt(p_phys);

        g_tx_descs[i * 2].buffer_addr = p_phys;
        g_tx_descs[i * 2].cmd = 0;
        g_tx_descs[i * 2].status = E1000_TXD_STAT_DD;
        g_tx_buf_virt[i * 2] = p_virt;

        g_tx_descs[i * 2 + 1].buffer_addr = p_phys + E1000_TX_BUFFER_SIZE;
        g_tx_descs[i * 2 + 1].cmd = 0;
        g_tx_descs[i * 2 + 1].status = E1000_TXD_STAT_DD;
        g_tx_buf_virt[i * 2 + 1] = p_virt + E1000_TX_BUFFER_SIZE;
    }

    g_tx_cur = 0;
    e1000_write32(E1000_REG_TDBAL, (uint32_t) (g_tx_ring_phys & 0xFFFFFFFF));
    e1000_write32(E1000_REG_TDBAH, (uint32_t) (g_tx_ring_phys >> 32));
    e1000_write32(E1000_REG_TDLEN, E1000_NUM_TX_DESC * sizeof(e1000_tx_desc_t));
    e1000_write32(E1000_REG_TDH, 0);
    e1000_write32(E1000_REG_TDT, 0);
    e1000_write32(E1000_REG_TIPG, (10 & 0x3FF) | ((8 & 0x3FF) << 10) | ((10 & 0x3FF) << 20));
    e1000_write32(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                                 (0x0F << E1000_TCTL_CT_SHIFT) |
                                 (0x40 << E1000_TCTL_COLD_SHIFT) |
                                 E1000_TCTL_RTLC);

    net_driver_register(&g_e1000_net_ops);
    g_net_registered = true;
    __atomic_store_n(&g_ready, true, __ATOMIC_RELEASE);

    g_irq_line = dev->irq_line;
    request_irq(g_irq_line, e1000_irq, NULL);
    g_irq_registered = true;

    e1000_write32(E1000_REG_IMS, E1000_ICR_RXT0 | E1000_ICR_RXO |
                                 E1000_ICR_RXDMT0 | E1000_ICR_RXSEQ |
                                 E1000_ICR_LSC | E1000_ICR_TXDW |
                                 E1000_ICR_TXQE);

    return 0;
}

static int e1000_init(void) {
    const pci_dev_t *target = NULL;
    for (int i = 0; i < g_pci_ndevs; i++) {
        if (g_pci_devs[i].vendor == INTEL_VENDOR_ID &&
            e1000_is_supported_device(g_pci_devs[i].device)) {
            target = &g_pci_devs[i];
            break;
        }
    }

    if (!target) return -ENODEV;

    int ret = e1000_init_hw(target);
    if (ret < 0) {
        e1000_stop();
        return ret;
    }

    return 0;
}

static void e1000_exit(void) {
    e1000_stop();
}

module_init(e1000_init);
module_exit(e1000_exit);
