#include "ahci.h"
#include "../arch/x86_64/cpu.h"
#include "../arch/x86_64/pit.h"
#include "../arch/x86_64/spinlock.h"
#include "../lib/log.h"
#include "../lib/printf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../mm/kmemleak.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../proc/proc.h"
#include "../block/block.h"
#include "../bus/pci/pci.h"

#define PCI_CLASS_STORAGE 0x01
#define PCI_SUBCLASS_SATA 0x06
#define PCI_PROGIF_AHCI 0x01
#define PCI_CMD_MEM 0x0002u
#define PCI_CMD_BUS_MASTER 0x0004u

#define GHC_AE (1u << 31)
#define GHC_RESET (1u << 0)

#define PORT_CMD_ST (1u << 0)
#define PORT_CMD_FRE (1u << 4)
#define PORT_CMD_FR (1u << 14)
#define PORT_CMD_CR (1u << 15)

#define PORT_TFD_BSY (1u << 7)
#define PORT_TFD_DRQ (1u << 3)
#define PORT_TFD_ERR (1u << 0)

#define SSTS_DET_MASK 0x0Fu
#define SSTS_DET_PHYOK 3u
#define SCTL_DET_MASK 0x0Fu
#define SCTL_DET_INIT 0x01u

#define PORT_IS_TFES (1u << 30)
#define PORT_IS_HBFS (1u << 29)
#define PORT_IS_HBDS (1u << 28)
#define PORT_IS_IFS (1u << 27)
#define PORT_IS_FATAL (PORT_IS_TFES | PORT_IS_HBFS | PORT_IS_HBDS | PORT_IS_IFS)

#define SIG_ATA 0x00000101u
#define SIG_ATAPI 0xEB140101u

#define ATA_CMD_READ_DMA_EXT 0x25u
#define ATA_CMD_WRITE_DMA_EXT 0x35u
#define ATA_CMD_FLUSH_EXT 0xEAu
#define ATA_CMD_IDENTIFY 0xECu
#define ATA_DEV_LBA 0x40u

#define FIS_TYPE_REG_H2D 0x27u

#define AHCI_MMIO_VBASE 0xffff930000000000ULL
#define AHCI_MMIO_PAGES 16

#define AHCI_MAX_PRDT 64u
// a command table is one page and can safely carry 64 prdt entries, in the
// worst case each entry covers one non-contiguous 4kb page, 512 sectors
// (256kb) need 64 entries
#define AHCI_MAX_SECTORS_PER_CMD ((AHCI_MAX_PRDT * PAGE_SIZE) / 512u)
#define AHCI_MAX_PORTS 32

typedef volatile struct {
    uint32_t clb, clbu;
    uint32_t fb, fbu;
    uint32_t is, ie, cmd, rsv0;
    uint32_t tfd, sig;
    uint32_t ssts, sctl, serr, sact, ci, sntf, fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} hba_port_t;

typedef volatile struct {
    uint32_t cap, ghc, is, pi, vs;
    uint32_t ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    uint8_t rsv[0xa0u - 0x2cu];
    uint8_t vendor[0x100u - 0xa0u];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint16_t flags;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t rsv[4];
} __attribute__((packed)) hba_cmd_hdr_t;

typedef struct {
    uint32_t dba, dbau;
    uint32_t rsv0;
    uint32_t dbc;
} __attribute__((packed)) hba_prdt_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_t prdt[1];
} __attribute__((packed)) hba_cmd_tbl_t;

_Static_assert(__builtin_offsetof(hba_cmd_tbl_t, prdt) +
                       AHCI_MAX_PRDT * sizeof(hba_prdt_t) <= PAGE_SIZE,
               "AHCI command table exceeds its DMA page");

typedef struct {
    hba_port_t *regs;
    hba_cmd_hdr_t *cmdlist;
    uint64_t cmdlist_phys;
    uint8_t *fis_buf;
    uint64_t fis_phys;
    hba_cmd_tbl_t *cmdtbl;
    uint64_t cmdtbl_phys;
    uint8_t *dma_buf;
    uint64_t dma_phys;
    uint64_t disk_sectors;
    char disk_model[41];
    bool present;
    bool busy;
    bool command_pending;
    uint64_t owner_waiters;
    spinlock_t lock;
} ahci_port_t;

static hba_mem_t *g_hba = NULL;
static ahci_port_t g_ports[AHCI_MAX_PORTS];
static bool g_ready = false;
static int g_port_count = 0;

static void wake_proc(proc_t *p) {
    if (p && __sync_bool_compare_and_swap(&p->state, PROC_WAITING, PROC_READY))
        proc_set_ready(p);
}

/* A port still has one command table, but contenders sleep instead of burning
 * another CPU on the port spinlock while the device is busy. */
static void port_acquire(ahci_port_t *ap) {
    proc_t *self = g_current_proc;
    for (;;) {
        uint64_t flags = irq_save();
        spin_lock(&ap->lock);
        if (!ap->busy) {
            ap->busy = true;
            if (self) ap->owner_waiters &= ~(1ULL << proc_slot(self));
            spin_unlock(&ap->lock);
            irq_restore(flags);
            return;
        }
        if (!self || !self->pid) {
            spin_unlock(&ap->lock);
            irq_restore(flags);
            cpu_relax();
            continue;
        }
        ap->owner_waiters |= 1ULL << proc_slot(self);
        self->state = PROC_WAITING;
        spin_unlock(&ap->lock);
        irq_restore(flags);
        sched_block_current();
    }
}

static void port_release(ahci_port_t *ap) {
    uint64_t flags = irq_save();
    spin_lock(&ap->lock);
    ap->busy = false;
    uint64_t waiters = ap->owner_waiters;
    ap->owner_waiters = 0;
    while (waiters) {
        int slot = __builtin_ctzll(waiters);
        wake_proc(&g_proctable[slot]);
        waiters &= waiters - 1;
    }
    spin_unlock(&ap->lock);
    irq_restore(flags);
}

/* The generic legacy-IRQ layer currently stores one handler per PIC line,
 * while PCI INTx lines are shared (and UIO may mask them).  Sleeping for one
 * scheduler tick is therefore safer than wiring AHCI to that API, and still
 * removes the old CPU-burning command loop. */
static int command_wait(ahci_port_t *ap, uint64_t timeout_ms) {
    hba_port_t *p = ap->regs;
    proc_t *self = g_current_proc;
    if (!self || !self->pid) {
        uint32_t spins = 30000000u;
        while ((p->ci & 1u) && !(p->is & PORT_IS_FATAL) && spins--) cpu_relax();
        return ((p->ci & 1u) || (p->is & PORT_IS_FATAL) || (p->tfd & PORT_TFD_ERR)) ? -1 : 0;
    }

    uint64_t deadline = g_ticks + timeout_ms;
    for (;;) {
        uint64_t flags = irq_save();
        spin_lock(&ap->lock);
        uint32_t status = p->is;
        bool done = !(p->ci & 1u) || (status & PORT_IS_FATAL);
        if (done || g_ticks >= deadline) {
            ap->command_pending = false;
            spin_unlock(&ap->lock);
            irq_restore(flags);
            self->wakeup_tick = 0;
            if (g_ticks >= deadline && (p->ci & 1u)) return -1;
            return ((status & PORT_IS_FATAL) || (p->tfd & PORT_TFD_ERR)) ? -1 : 0;
        }

        self->wakeup_tick = g_ticks + PIT_TICK_MS;
        if (self->wakeup_tick > deadline) self->wakeup_tick = deadline;
        proc_set_timer(self);
        self->state = PROC_WAITING;
        spin_unlock(&ap->lock);
        irq_restore(flags);
        sched_block_current();
        self->wakeup_tick = 0;
    }
}

static void port_stop(hba_port_t *p) {
    p->cmd &= ~(uint32_t) PORT_CMD_ST;
    uint32_t to = 500000u;
    while ((p->cmd & PORT_CMD_CR) && to--) cpu_relax();
    p->cmd &= ~(uint32_t) PORT_CMD_FRE;
    to = 500000u;
    while ((p->cmd & PORT_CMD_FR) && to--) cpu_relax();
}

static void port_start(hba_port_t *p) {
    uint32_t to = 500000u;
    while ((p->tfd & (PORT_TFD_BSY | PORT_TFD_DRQ)) && to--) cpu_relax();
    p->cmd |= PORT_CMD_FRE;
    p->cmd |= PORT_CMD_ST;
}

static void port_comreset(hba_port_t *p) {
    port_stop(p);
    p->sctl = (p->sctl & ~(uint32_t) SCTL_DET_MASK) | SCTL_DET_INIT;
    for (volatile uint32_t i = 0; i < 100000u; i++) cpu_relax();
    p->sctl &= ~(uint32_t) SCTL_DET_MASK;
    uint32_t to = 1000000u;
    while ((p->ssts & SSTS_DET_MASK) != SSTS_DET_PHYOK && to--) cpu_relax();
    p->serr = 0xFFFFFFFFu;
    p->is = 0xFFFFFFFFu;
    port_start(p);
}

static bool port_identify(int idx) {
    ahci_port_t *ap = &g_ports[idx];
    hba_port_t *p = ap->regs;
    hba_cmd_tbl_t *tbl = ap->cmdtbl;
    hba_cmd_hdr_t *hdr = &ap->cmdlist[0];

    memset(tbl, 0, sizeof(hba_cmd_tbl_t));
    tbl->cfis[0] = FIS_TYPE_REG_H2D;
    tbl->cfis[1] = 0x80u;
    tbl->cfis[2] = ATA_CMD_IDENTIFY;

    tbl->prdt[0].dba = (uint32_t) (ap->dma_phys & 0xFFFFFFFFu);
    tbl->prdt[0].dbau = (uint32_t) (ap->dma_phys >> 32);
    tbl->prdt[0].dbc = 511u;

    hdr->flags = 5u;
    hdr->prdtl = 1;
    hdr->prdbc = 0;

    __asm__ volatile("" ::: "memory");
    p->is = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;
    p->ci = 1u;

    uint32_t timeout = 5000000u;
    while (timeout--) {
        if (!(p->ci & 1u)) break;
        if (p->is & PORT_IS_FATAL) return false;
        cpu_relax();
    }
    if ((p->ci & 1u) || (p->tfd & PORT_TFD_ERR)) return false;

    const uint16_t *id = (const uint16_t *) (const void *) ap->dma_buf;

    ap->disk_sectors = (uint64_t) id[100] | ((uint64_t) id[101] << 16) |
                       ((uint64_t) id[102] << 32) | ((uint64_t) id[103] << 48);
    if (!ap->disk_sectors) ap->disk_sectors = (uint64_t) id[60] | ((uint64_t) id[61] << 16);

    for (int i = 0; i < 20; i++) {
        ap->disk_model[i * 2] = (char) (id[27 + i] >> 8);
        ap->disk_model[i * 2 + 1] = (char) (id[27 + i] & 0xFFu);
    }
    ap->disk_model[40] = '\0';
    for (int i = 39; i >= 0 && ap->disk_model[i] == ' '; i--) ap->disk_model[i] = '\0';

    return true;
}

static bool port_init(int idx) {
    hba_port_t *p = &g_hba->ports[idx];
    ahci_port_t *ap = &g_ports[idx];

    log_info("AHCI: port %d  initial SSTS=0x%08x  SIG=0x%08x", idx, p->ssts, p->sig);

    port_stop(p);
    p->sctl = (p->sctl & ~(uint32_t) SCTL_DET_MASK) | SCTL_DET_INIT;
    for (volatile uint32_t i = 0; i < 100000u; i++) cpu_relax();
    p->sctl &= ~(uint32_t) SCTL_DET_MASK;
    uint32_t to = 100000u;
    while ((p->ssts & SSTS_DET_MASK) != SSTS_DET_PHYOK && to--) cpu_relax();
    log_info("AHCI: port %d  after COMRESET SSTS=0x%08x  SIG=0x%08x", idx, p->ssts, p->sig);
    if ((p->ssts & SSTS_DET_MASK) != SSTS_DET_PHYOK) return false;
    p->serr = 0xFFFFFFFFu;
    p->is = 0xFFFFFFFFu;

    port_stop(p);

    ap->cmdlist_phys = (uint64_t) pmm_alloc_zeroed();
    if (!ap->cmdlist_phys) return false;
    ap->cmdlist = (hba_cmd_hdr_t *) phys_to_virt(ap->cmdlist_phys);
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm((void *) ap->cmdlist_phys);
#endif

    ap->fis_phys = (uint64_t) pmm_alloc_zeroed();
    if (!ap->fis_phys) return false;
    ap->fis_buf = (uint8_t *) phys_to_virt(ap->fis_phys);
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm((void *) ap->fis_phys);
#endif

    ap->cmdtbl_phys = (uint64_t) pmm_alloc_zeroed();
    if (!ap->cmdtbl_phys) return false;
    ap->cmdtbl = (hba_cmd_tbl_t *) phys_to_virt(ap->cmdtbl_phys);
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm((void *) ap->cmdtbl_phys);
#endif

    ap->dma_phys = (uint64_t) pmm_alloc_zeroed();
    if (!ap->dma_phys) return false;
    ap->dma_buf = (uint8_t *) phys_to_virt(ap->dma_phys);
#ifdef CONFIG_KMEMLEAK
    kmemleak_page_perm((void *) ap->dma_phys);
#endif

    ap->cmdlist[0].ctba = (uint32_t) (ap->cmdtbl_phys & 0xFFFFFFFFu);
    ap->cmdlist[0].ctbau = (uint32_t) (ap->cmdtbl_phys >> 32);

    p->clb = (uint32_t) (ap->cmdlist_phys & 0xFFFFFFFFu);
    p->clbu = (uint32_t) (ap->cmdlist_phys >> 32);
    p->fb = (uint32_t) (ap->fis_phys & 0xFFFFFFFFu);
    p->fbu = (uint32_t) (ap->fis_phys >> 32);

    p->is = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;

    ap->regs = p;
    ap->present = true;
    ap->busy = false;
    ap->command_pending = false;
    ap->owner_waiters = 0;

    port_start(p);

    if (port_identify(idx)) {
        uint64_t mib = ap->disk_sectors / 2048u;
        log_info("AHCI: port %d  \"%s\"  %lu sectors  (%lu MiB)", idx, ap->disk_model,
                 ap->disk_sectors, mib);
    } else {
        log_warn("AHCI: port %d  IDENTIFY failed, capacity unknown", idx);
    }

    return true;
}

static int ahci_block_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    int port = (int) (uintptr_t) dev->priv;
    return ahci_read(port, lba, count, buf);
}

static int ahci_block_write(struct block_device *dev, uint64_t lba, uint32_t count,
                            const void *buf) {
    int port = (int) (uintptr_t) dev->priv;
    return ahci_write(port, lba, count, buf);
}

static int ahci_block_flush(struct block_device *dev) {
    int port = (int) (uintptr_t) dev->priv;
    return ahci_flush(port);
}

bool ahci_init(void) {
    pci_dev_t *pci = NULL;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class == PCI_CLASS_STORAGE && d->subclass == PCI_SUBCLASS_SATA &&
            d->prog_if == PCI_PROGIF_AHCI) {
            pci = d;
            break;
        }
    }
    if (!pci) {
        log_warn("AHCI: no controller found");
        return false;
    }

    uint32_t cmd = pci_read32(pci->bus, pci->dev, pci->fn, 0x04);
    cmd |= PCI_CMD_MEM | PCI_CMD_BUS_MASTER;
    pci_write32(pci->bus, pci->dev, pci->fn, 0x04, cmd);

    uint32_t bar5_raw = pci_read32(pci->bus, pci->dev, pci->fn, 0x24);
    if (bar5_raw & 1u) {
        log_error("AHCI: ABAR is I/O BAR — not supported");
        return false;
    }
    uint64_t abar_phys = bar5_raw & ~0xFULL;
    if ((bar5_raw >> 1) & 2u) {
        uint32_t bar5_hi = pci_read32(pci->bus, pci->dev, pci->fn, 0x28);
        abar_phys |= (uint64_t) bar5_hi << 32;
    }
    if (!abar_phys) {
        log_error("AHCI: ABAR is zero");
        return false;
    }

    uint64_t abar_page = abar_phys & PAGE_MASK;
    for (int i = 0; i < AHCI_MMIO_PAGES; i++) {
        int r = vmm_map(&g_kernel_space, AHCI_MMIO_VBASE + (uint64_t) i * PAGE_SIZE,
                        abar_page + (uint64_t) i * PAGE_SIZE,
                        VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_PCD);
        if (r) {
            log_error("AHCI: vmm_map failed");
            return false;
        }
    }
    g_hba = (hba_mem_t *) (AHCI_MMIO_VBASE + (abar_phys & (PAGE_SIZE - 1)));

    log_info("AHCI: ABAR phys=0x%016lx virt=%p", abar_phys, (void *) g_hba);

    g_hba->ghc |= GHC_AE;

    g_hba->ghc |= GHC_RESET;
    uint32_t to = 2000000u;
    while ((g_hba->ghc & GHC_RESET) && to--) cpu_relax();
    g_hba->ghc |= GHC_AE;

    uint32_t pi = g_hba->pi;
    log_info("AHCI: CAP=0x%08x  PI=0x%08x", g_hba->cap, pi);

    static struct block_device_ops ahci_ops = {
        ahci_block_read,
        ahci_block_write,
        ahci_block_flush,
    };
    int found = 0;
    int disk_idx = 0;
    for (int i = 0; i < 32; i++) {
        if (!(pi & (1u << i))) continue;
        if (port_init(i)) {
            struct block_device *bd = (struct block_device *) kmalloc(sizeof(struct block_device));
            if (bd) {
                memset(bd, 0, sizeof(*bd));
                snprintf(bd->name, sizeof(bd->name), "sd%c", 'a' + disk_idx);
                bd->sectors = g_ports[i].disk_sectors;
                bd->sector_size = 512;
                bd->ops = &ahci_ops;
                bd->priv = (void *) (uintptr_t) i;
                bd->offset_lba = 0;
                bd->parent = NULL;
                block_register(bd);
            }
            disk_idx++;
            found++;
        }
    }

    g_ready = (found > 0);
    g_port_count = found;
    log_info("AHCI: %d disk(s) ready", found);
    return g_ready;
}

bool ahci_ready(void) { return g_ready; }
int ahci_port_count(void) { return g_port_count; }

int ahci_first_disk(void) {
    for (int i = 0; i < AHCI_MAX_PORTS; i++)
        if (g_ports[i].present) return i;
    return -1;
}

uint64_t ahci_disk_sectors(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !g_ports[port].present) return 0;
    return g_ports[port].disk_sectors;
}

void ahci_disk_model(int port, char *buf, int len) {
    if (!buf || len <= 0) return;
    if (port < 0 || port >= AHCI_MAX_PORTS || !g_ports[port].present) {
        buf[0] = '\0';
        return;
    }
    const char *src = g_ports[port].disk_model;
    int n = 0;
    while (n < len - 1 && *src) buf[n++] = *src++;
    buf[n] = '\0';
}

static uint64_t dma_page_phys(uint64_t virt) {
    uint64_t direct_len = pmm_total_pages() * PAGE_SIZE;
    if (virt >= g_hhdm_offset && virt - g_hhdm_offset < direct_len)
        return virt - g_hhdm_offset;
    return vmm_virt_to_phys(&g_kernel_space, virt);
}

static int build_prdt(hba_cmd_tbl_t *tbl, const void *buffer, uint32_t bytes) {
    uint64_t virt = (uint64_t) (uintptr_t) buffer;
    uint16_t entries = 0;

    while (bytes) {
        uint32_t page_off = (uint32_t) (virt & (PAGE_SIZE - 1));
        uint32_t chunk = (uint32_t) PAGE_SIZE - page_off;
        if (chunk > bytes) chunk = bytes;

        uint64_t phys_page = dma_page_phys(virt & PAGE_MASK);
        if (!phys_page) return -1;
        uint64_t phys = phys_page + page_off;

        if (entries) {
            hba_prdt_t *prev = &tbl->prdt[entries - 1];
            uint64_t prev_phys = (uint64_t) prev->dba | ((uint64_t) prev->dbau << 32);
            uint32_t prev_bytes = (prev->dbc & 0x3FFFFFu) + 1u;
            if (prev_phys + prev_bytes == phys && prev_bytes + chunk <= 0x400000u) {
                prev->dbc += chunk;
                virt += chunk;
                bytes -= chunk;
                continue;
            }
        }

        if (entries == AHCI_MAX_PRDT) return -1;
        hba_prdt_t *entry = &tbl->prdt[entries++];
        entry->dba = (uint32_t) phys;
        entry->dbau = (uint32_t) (phys >> 32);
        entry->rsv0 = 0;
        entry->dbc = chunk - 1u;

        virt += chunk;
        bytes -= chunk;
    }
    return entries;
}

static int do_command(ahci_port_t *ap, uint64_t lba, uint32_t sectors, bool write,
                      const void *buffer) {
    hba_port_t *p = ap->regs;
    hba_cmd_tbl_t *tbl = ap->cmdtbl;
    hba_cmd_hdr_t *hdr = &ap->cmdlist[0];

    memset(tbl, 0, PAGE_SIZE);

    int prdt_count = build_prdt(tbl, buffer, sectors * 512u);
    if (prdt_count < 0) {
        log_error("AHCI: cannot map DMA buffer %p (%u sectors)", buffer, sectors);
        return -1;
    }

    tbl->cfis[0] = FIS_TYPE_REG_H2D;
    tbl->cfis[1] = 0x80u;
    tbl->cfis[2] = write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT;
    tbl->cfis[3] = 0;
    tbl->cfis[4] = (uint8_t) (lba & 0xFFu);
    tbl->cfis[5] = (uint8_t) ((lba >> 8) & 0xFFu);
    tbl->cfis[6] = (uint8_t) ((lba >> 16) & 0xFFu);
    tbl->cfis[7] = ATA_DEV_LBA;
    tbl->cfis[8] = (uint8_t) ((lba >> 24) & 0xFFu);
    tbl->cfis[9] = (uint8_t) ((lba >> 32) & 0xFFu);
    tbl->cfis[10] = (uint8_t) ((lba >> 40) & 0xFFu);
    tbl->cfis[11] = 0;
    tbl->cfis[12] = (uint8_t) (sectors & 0xFFu);
    tbl->cfis[13] = (uint8_t) ((sectors >> 8) & 0xFFu);
    tbl->cfis[14] = 0;
    tbl->cfis[15] = 0;

    uint16_t flags = 5u;
    if (write) flags |= (uint16_t) (1u << 6);
    hdr->flags = flags;
    hdr->prdtl = (uint16_t) prdt_count;
    hdr->prdbc = 0;

    __asm__ volatile("" ::: "memory");

    p->is = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;
    spin_lock(&ap->lock);
    ap->command_pending = true;
    spin_unlock(&ap->lock);
    p->ci = 1u;

    if (command_wait(ap, 30000u) < 0) {
        log_error("AHCI: timeout  lba=%lu  sectors=%u", lba, sectors);
        port_comreset(p);
        return -1;
    }
    if (p->tfd & PORT_TFD_ERR) {
        log_error("AHCI: ATA error  TFD=0x%08x", p->tfd);
        return -1;
    }
    return 0;
}

int ahci_read(int port, uint64_t lba, uint32_t count, void *buf) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !g_ports[port].present) return -1;
    ahci_port_t *ap = &g_ports[port];
    port_acquire(ap);
    uint8_t *dst = (uint8_t *) buf;
    uint32_t rem = count;

    while (rem > 0) {
        uint32_t batch = rem < AHCI_MAX_SECTORS_PER_CMD ? rem : AHCI_MAX_SECTORS_PER_CMD;
        if (do_command(ap, lba, batch, false, dst) < 0) {
            port_release(ap);
            return -1;
        }
        dst += batch * 512u;
        lba += batch;
        rem -= batch;
    }
    port_release(ap);
    return 0;
}

int ahci_flush(int port) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !g_ports[port].present) return -1;
    ahci_port_t *ap = &g_ports[port];
    port_acquire(ap);
    hba_port_t *p = ap->regs;
    hba_cmd_tbl_t *tbl = ap->cmdtbl;
    hba_cmd_hdr_t *hdr = &ap->cmdlist[0];

    memset(tbl, 0, sizeof(hba_cmd_tbl_t));
    tbl->cfis[0] = FIS_TYPE_REG_H2D;
    tbl->cfis[1] = 0x80u;
    tbl->cfis[2] = ATA_CMD_FLUSH_EXT;

    hdr->flags = 5u;
    hdr->prdtl = 0;
    hdr->prdbc = 0;

    __asm__ volatile("" ::: "memory");
    p->is = 0xFFFFFFFFu;
    p->serr = 0xFFFFFFFFu;
    spin_lock(&ap->lock);
    ap->command_pending = true;
    spin_unlock(&ap->lock);
    p->ci = 1u;

    if (command_wait(ap, 30000u) < 0) {
        log_error("AHCI: flush timeout on port %d", port);
        port_comreset(p);
        port_release(ap);
        return -1;
    }
    port_release(ap);
    return 0;
}

int ahci_write(int port, uint64_t lba, uint32_t count, const void *buf) {
    if (port < 0 || port >= AHCI_MAX_PORTS || !g_ports[port].present) return -1;
    ahci_port_t *ap = &g_ports[port];
    port_acquire(ap);
    const uint8_t *src = (const uint8_t *) buf;
    uint32_t rem = count;

    while (rem > 0) {
        uint32_t batch = rem < AHCI_MAX_SECTORS_PER_CMD ? rem : AHCI_MAX_SECTORS_PER_CMD;
        if (do_command(ap, lba, batch, true, src) < 0) {
            port_release(ap);
            return -1;
        }
        src += batch * 512u;
        lba += batch;
        rem -= batch;
    }
    port_release(ap);
    return 0;
}
