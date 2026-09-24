#include "usb_glue.h"

#include "../arch/x86_64/cpu.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../proc/proc.h"
#include "../tty/tty.h"
#include "../tty/vt.h"

void usb_msleep(uint64_t ms) {
    if (ms == 0) return;
    proc_t *self = g_current_proc;

    if (!self || !self->pid) {
        uint64_t deadline = g_ticks + ms;
        while ((int64_t) (deadline - g_ticks) > 0) cpu_relax();
        return;
    }
    uint64_t deadline = g_ticks + ms;
    for (;;) {
        uint64_t flags = irq_save();
        if (g_ticks >= deadline) {
            irq_restore(flags);
            return;
        }
        self->wakeup_tick = g_ticks + PIT_TICK_MS;
        if (self->wakeup_tick > deadline) self->wakeup_tick = deadline;
        proc_set_timer(self);
        self->state = PROC_WAITING;
        irq_restore(flags);
        sched_block_current();
    }
}

static uint64_t g_tsc_per_us;
static spinlock_t g_udelay_lock = SPINLOCK_INIT;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)::"memory");
    return ((uint64_t) hi << 32) | lo;
}

static void usb_udelay_calibrate(void) {
    spin_lock(&g_udelay_lock);
    if (g_tsc_per_us == 0) {
        uint64_t t0 = g_ticks;
        while (g_ticks == t0) cpu_relax();
        t0 = g_ticks;
        uint64_t c0 = rdtsc();
        while (g_ticks == t0) cpu_relax();
        uint64_t cycles = rdtsc() - c0;
        uint64_t per_us = cycles / (PIT_TICK_MS * 1000);
        if (per_us) g_tsc_per_us = per_us;
    }
    spin_unlock(&g_udelay_lock);
}

void usb_udelay(uint64_t us) {
    if (g_tsc_per_us == 0) usb_udelay_calibrate();
    if (g_tsc_per_us == 0) {
        usb_msleep((us + 999) / 1000);
        return;
    }
    uint64_t end = rdtsc() + us * g_tsc_per_us;
    while (rdtsc() < end) cpu_relax();
}

int usb_pci_for_each(uint8_t class_, uint8_t subclass, int prog_if,
                     usb_pci_probe_fn probe) {
    int n = 0;
    for (int i = 0; i < g_pci_ndevs; i++) {
        pci_dev_t *d = &g_pci_devs[i];
        if (d->class != class_ || d->subclass != subclass) continue;
        if (prog_if >= 0 && d->prog_if != (uint8_t) prog_if) continue;
        probe(d);
        n++;
    }
    return n;
}

#define USB_MMIO_VBASE 0xffff950000000000ULL
#define USB_MMIO_SLOT  (64ULL * 1024 * 1024)
#define USB_MMIO_SLOTS 16

static spinlock_t g_mmio_lock = SPINLOCK_INIT;

volatile void *usb_mmio_map_region(uintptr_t phys, size_t size) {
    static int next_slot;
    if (size == 0) return NULL;

    uint64_t pages = ((uint64_t) size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t base = phys & ~(uint64_t) (PAGE_SIZE - 1);
    uint64_t page_off = phys - base;

    uint64_t flags = irq_save();
    spin_lock(&g_mmio_lock);
    if (next_slot >= USB_MMIO_SLOTS) {
        spin_unlock(&g_mmio_lock);
        irq_restore(flags);
        klog_printf(KLOG_WARN, "W: [usb] out of MMIO VA slots\n");
        return NULL;
    }
    uint64_t vbase = USB_MMIO_VBASE + (uint64_t) next_slot * USB_MMIO_SLOT;
    for (uint64_t i = 0; i < pages; i++) {
        if (vmm_map(&g_kernel_space, vbase + i * PAGE_SIZE,
                    base + i * PAGE_SIZE,
                    VMM_PRESENT | VMM_WRITE | VMM_NX | VMM_PCD) != 0) {
            spin_unlock(&g_mmio_lock);
            irq_restore(flags);
            klog_printf(KLOG_WARN, "W: [usb] vmm_map failed for MMIO 0x%llx\n",
                        (unsigned long long) phys);
            return NULL;
        }
    }
    next_slot++;
    spin_unlock(&g_mmio_lock);
    irq_restore(flags);
    return (volatile void *) (vbase + page_off);
}

void usb_mmio_unmap_region(volatile void *virt, size_t size) {
    (void) virt;
    (void) size;
}

#define LOW_ARENA_CHUNK_PAGES 16
#define LOW_ARENA_MAX_CHUNKS 48
#define LOW_ARENA_LIMIT 0x100000000ULL

typedef struct low_chunk {
    uintptr_t phys;
    size_t pages;
    size_t free_off;
    struct low_chunk *next;
} low_chunk_t;

typedef struct low_free {
    size_t pages;
    struct low_free *next;
} low_free_t;

static low_chunk_t g_low_chunks[LOW_ARENA_MAX_CHUNKS];
static int g_low_nchunks;
static low_free_t *g_low_freelist;
static spinlock_t g_low_lock = SPINLOCK_INIT;

static bool low_chunk_add(void) {
    if (g_low_nchunks >= LOW_ARENA_MAX_CHUNKS) return false;
    void *phys = pmm_dma32_reserve_alloc(LOW_ARENA_CHUNK_PAGES);
    if (!phys) return false;
    low_chunk_t *c = &g_low_chunks[g_low_nchunks++];
    c->phys = (uintptr_t)phys;
    c->pages = LOW_ARENA_CHUNK_PAGES;
    c->free_off = 0;
    c->next = NULL;
    return true;
}

static void *low_alloc(size_t pages) {

    low_free_t **pp = &g_low_freelist;
    for (low_free_t *f = g_low_freelist; f; f = f->next) {
        if (f->pages >= pages) {
            if (f->pages > pages) {

                uint8_t *base = (uint8_t *) f;
                low_free_t *tail = (low_free_t *) (base + pages * 4096);
                tail->pages = f->pages - pages;
                tail->next = f->next;
                *pp = tail;
            } else {
                *pp = f->next;
            }
            return f;
        }
        pp = &f->next;
    }

    for (int i = 0; i < g_low_nchunks; i++) {
        low_chunk_t *c = &g_low_chunks[i];
        if (c->pages - c->free_off >= pages) {
            void *v = (uint8_t *) phys_to_virt(c->phys) + c->free_off * 4096;
            c->free_off += pages;
            return v;
        }
    }

    if (pages > LOW_ARENA_CHUNK_PAGES) return NULL;
    if (!low_chunk_add()) return NULL;
    low_chunk_t *c = &g_low_chunks[g_low_nchunks - 1];
    void *v = (uint8_t *) phys_to_virt(c->phys);
    c->free_off += pages;
    return v;
}

static bool low_owns(const void *virt) {
    uintptr_t v = (uintptr_t) virt;
    for (int i = 0; i < g_low_nchunks; i++) {
        uintptr_t base = (uintptr_t) phys_to_virt(g_low_chunks[i].phys);
        if (v >= base && v < base + g_low_chunks[i].pages * 4096) return true;
    }
    return false;
}

void *dma_alloc_coherent_low(size_t size, uintptr_t *phys_out) {
    if (size == 0) return NULL;
    size_t pages = (size + 4095) / 4096;

    uint64_t flags = irq_save();
    spin_lock(&g_low_lock);
    void *v = low_alloc(pages);
    spin_unlock(&g_low_lock);
    irq_restore(flags);

    if (!v) {
        klog_printf(KLOG_WARN, "W: [usb-dma] low alloc of %u bytes failed\n", (unsigned) size);
        return NULL;
    }
    memset(v, 0, pages * 4096);
    if (phys_out) *phys_out = virt_to_phys(v);
    return v;
}

void *dma_alloc_coherent(size_t size, uintptr_t *phys_out) {
    if (size == 0) return NULL;
    size_t pages = (size + 4095) / 4096;
    void *phys = (pages > 1) ? pmm_alloc_contiguous(pages) : pmm_alloc_zeroed();
    if (!phys) {
        klog_printf(KLOG_WARN, "W: [usb-dma] alloc of %u bytes failed\n", (unsigned) size);
        return NULL;
    }
    void *v = phys_to_virt((uintptr_t) phys);
    memset(v, 0, pages * 4096);
    if (phys_out) *phys_out = (uintptr_t) phys;
    return v;
}

void dma_free_coherent(void *virt, size_t size) {
    if (!virt || size == 0) return;
    size_t pages = (size + 4095) / 4096;

    if (low_owns(virt)) {
        uint64_t flags = irq_save();
        spin_lock(&g_low_lock);
        low_free_t *f = (low_free_t *) virt;
        f->pages = pages;
        f->next = g_low_freelist;
        g_low_freelist = f;
        spin_unlock(&g_low_lock);
        irq_restore(flags);
        return;
    }

    pmm_free_contiguous((void *) virt_to_phys(virt), pages);
}

void usb_input_report_key(uint8_t set1_scancode, bool ext, int pressed) {
    uint16_t lk = kbd_set1_to_linuxkey(set1_scancode, ext);
    if (!lk) return;
    input_push(INPUT_DEV_KBD, EV_KEY, lk, pressed ? 1 : 0);
    input_push(INPUT_DEV_KBD, EV_SYN, SYN_REPORT, 0);
}

void usb_mouse_inject_rel(int32_t dx, int32_t dy, bool btn_left, bool btn_right,
                          bool btn_middle, int32_t wheel)
{
    if (dx) input_push(INPUT_DEV_MOUSE, EV_REL, REL_X, dx);
    if (dy) input_push(INPUT_DEV_MOUSE, EV_REL, REL_Y, dy);
    if (wheel) input_push(INPUT_DEV_MOUSE, EV_REL, REL_WHEEL, wheel);

    static bool btns_prev[3];
    bool btns[3] = { btn_left, btn_right, btn_middle };
    for (int i = 0; i < 3; i++) {
        if (btns[i] != btns_prev[i])
            input_push(INPUT_DEV_MOUSE, EV_KEY, BTN_LEFT + i, btns[i] ? 1 : 0);
    }
    btns_prev[0] = btn_left; btns_prev[1] = btn_right; btns_prev[2] = btn_middle;

    input_push(INPUT_DEV_MOUSE, EV_SYN, SYN_REPORT, 0);
}
