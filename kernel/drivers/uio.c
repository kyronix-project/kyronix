#include "uio.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/pic.h"
#include "../fs/vfs.h"
#include "../lib/log.h"
#include "../lib/string.h"
#include "../lib/printf.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../proc/proc.h"

#define EINVAL 22
#define ENODEV 19
#define EPERM  1

static uio_dev_t g_uio_devs[PCI_MAX_DEVS];
static int       g_uio_ndevs;

static void irq_handler(int irq, void* arg)
{
    uio_dev_t* uio = (uio_dev_t*)arg;
    uio->irq_count++;
    pic_mask_irq((uint8_t)irq); /* driver re-enables via write() */
    if (uio->waiter && uio->waiter->state == PROC_WAITING)
        uio->waiter->state = PROC_READY;
}

static int64_t uio_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void)off;
    uio_dev_t* uio = (uio_dev_t*)n->data;
    if (!uio || len < 4) return -(int64_t)EINVAL;

    uio->waiter = g_current_proc;
    while (!uio->irq_count)
        sched_yield_blocking();
    uio->waiter = NULL;

    uint32_t cnt = uio->irq_count;
    uio->irq_count = 0;
    __builtin_memcpy(buf, &cnt, 4);
    return 4;
}

static int64_t uio_write(vfs_node_t* n, const char* buf, uint64_t len)
{
    (void)buf; (void)len;
    uio_dev_t* uio = (uio_dev_t*)n->data;
    if (!uio) return -(int64_t)EINVAL;
    pic_unmask_irq(uio->pdev->irq_line); /* re-arm the IRQ line */
    return (int64_t)len;
}

static int64_t uio_mmap(vfs_node_t* n, uint64_t off, uint64_t len,
                        uint64_t va, uint64_t vflags)
{
    uio_dev_t* uio = (uio_dev_t*)n->data;
    if (!uio) return -(int64_t)EINVAL;

    int bar_idx = (int)(off >> 12); /* offset N*PAGE_SIZE → BAR[N] */
    if (bar_idx >= 6) return -(int64_t)EINVAL;
    uint64_t phys = uio->pdev->bars[bar_idx];
    if (!phys) return -(int64_t)ENODEV;

    proc_t* p = g_current_proc;
    if (!p || !p->space) return -(int64_t)EINVAL;
    if (p->euid != 0) return -(int64_t)EPERM;

    uint64_t sz = uio->pdev->bar_sizes[bar_idx];
    if (!sz || sz > len) sz = len;
    sz = (sz + 0xFFF) & ~0xFFFULL;

    /* MMIO: present + user + write, no-exec, no cache (PAT/PCD/PWT = 0 for UC) */
    uint64_t flags = vflags | VMM_PRESENT | VMM_USER | VMM_WRITE;
    for (uint64_t o = 0; o < sz; o += 0x1000)
        vmm_map(p->space, va + o, phys + o, flags);
    return (int64_t)va;
}

void uio_init(void)
{
    g_uio_ndevs = 0;
    char path[32];
    for (int i = 0; i < g_pci_ndevs; i++) {
        uio_dev_t* uio = &g_uio_devs[g_uio_ndevs++];
        uio->pdev      = &g_pci_devs[i];
        uio->irq_count = 0;
        uio->waiter    = NULL;

        snprintf(path, sizeof(path), "/dev/uio%d", i);
        vfs_node_t* node = vfs_create_chr(path, uio_read, uio_write);
        if (!node) continue;
        node->mode     = S_IFCHR | 0600;
        node->data     = (uint8_t*)uio;
        node->chr_mmap = uio_mmap;

        uint8_t irq = uio->pdev->irq_line;
        if (irq && irq != 0xFF)
            request_irq(irq, irq_handler, uio);

        log_info("UIO: %s  %04x:%04x  irq=%u  bar0=0x%lx",
                 path, uio->pdev->vendor, uio->pdev->device,
                 irq, uio->pdev->bars[0]);
    }
    log_info("UIO: %d devices registered", g_uio_ndevs);
}
