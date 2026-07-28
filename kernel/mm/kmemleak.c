#include "kmemleak.h"
#ifdef CONFIG_KMEMLEAK

#include "heap.h"
#include "lib/kallsyms.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "pmm.h"
#include "proc/proc.h"
#include "vmm.h"

#define KMEMLEAK_MAX 8192
#define KMEMLEAK_PAGE_MAX 2048

typedef struct {
    void *ptr;
    uint64_t size;
    uint64_t backtrace[KMEMLEAK_BT_DEPTH];
    int bt_depth;
    int reachable;
    int used;
} kmemleak_entry_t;

typedef struct {
    uint64_t phys;
    uint64_t backtrace[KMEMLEAK_BT_DEPTH];
    int bt_depth;
    int reachable;
    int used;
    int perm;
} page_entry_t;

static kmemleak_entry_t g_entries[KMEMLEAK_MAX];
static page_entry_t g_pages[KMEMLEAK_PAGE_MAX];
static uint64_t g_max_phys;

extern uint8_t __data_start[], __data_end[];
extern uint8_t __bss_start[], __bss_end[];

static int is_kernel_addr(void *p) {
    if (!p) return 0;
    uint64_t v = (uint64_t) p;
    return v >= 0xffff800000000000ULL;
}

static int page_is_mapped(uint64_t virt);

static void capture_backtrace(uint64_t *bt, int *depth) {
    struct frame {
        struct frame *rbp;
        uint64_t ret;
    };
    struct frame *fp;
    __asm__("mov %%rbp, %0" : "=r"(fp));
    int i = 0;
    while (i < KMEMLEAK_BT_DEPTH) {
        if (!fp || !is_kernel_addr(fp)) break;
        if (!page_is_mapped((uint64_t) fp) || !page_is_mapped((uint64_t) fp + sizeof(struct frame)))
            break;
        bt[i++] = fp->ret;
        fp = fp->rbp;
    }
    *depth = i;
}

static int in_entries_range(const void *addr) {
    uint64_t a = (uint64_t) addr;
    uint64_t e0 = (uint64_t) g_entries;
    uint64_t e1 = e0 + sizeof(g_entries);
    uint64_t p0 = (uint64_t) g_pages;
    uint64_t p1 = p0 + sizeof(g_pages);
    return (a >= e0 && a < e1) || (a >= p0 && a < p1);
}

void kmemleak_track(void *ptr, uint64_t size) {
    if (!ptr) return;
    for (int i = 0; i < KMEMLEAK_MAX; i++) {
        if (!g_entries[i].used) {
            g_entries[i].ptr = ptr;
            g_entries[i].size = size;
            g_entries[i].reachable = 0;
            g_entries[i].used = 1;
            capture_backtrace(g_entries[i].backtrace, &g_entries[i].bt_depth);
            return;
        }
    }
}

void kmemleak_untrack(void *ptr) {
    if (!ptr) return;
    for (int i = 0; i < KMEMLEAK_MAX; i++) {
        if (g_entries[i].used && g_entries[i].ptr == ptr) {
            g_entries[i].used = 0;
            return;
        }
    }
}

void kmemleak_track_page(void *phys) {
    if (!phys) return;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) {
        if (!g_pages[i].used) {
            g_pages[i].phys = (uint64_t) phys;
            g_pages[i].reachable = 0;
            g_pages[i].used = 1;
            g_pages[i].perm = 0;
            capture_backtrace(g_pages[i].backtrace, &g_pages[i].bt_depth);
            return;
        }
    }
}

void kmemleak_page_perm(void *phys) {
    if (!phys) return;
    uint64_t p = (uint64_t) phys;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) {
        if (g_pages[i].used && g_pages[i].phys == p) {
            g_pages[i].perm = 1;
            return;
        }
    }
}

void kmemleak_untrack_page(void *phys) {
    if (!phys) return;
    uint64_t p = (uint64_t) phys;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) {
        if (g_pages[i].used && g_pages[i].phys == p) {
            g_pages[i].used = 0;
            return;
        }
    }
}

static int page_is_mapped(uint64_t virt) {
    if (!g_kernel_space.pml4_phys) return 1;
    return vmm_virt_to_phys(&g_kernel_space, virt) != 0;
}

static void try_mark_page(uint64_t phys) {
    if (phys >= g_max_phys) return;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) {
        if (g_pages[i].used && !g_pages[i].reachable && g_pages[i].phys == phys) {
            g_pages[i].reachable = 1;
        }
    }
}

static void scan_region(const uint8_t *start, const uint8_t *end) {
    if (!start || !end || start >= end) return;
    uint64_t a = (uint64_t) start;
    uint64_t b = (uint64_t) end;
    a &= ~7ULL;
    const uint64_t *p = (const uint64_t *) a;
    const uint64_t *limit = (const uint64_t *) b;
    while (p < limit) {
        if (!page_is_mapped((uint64_t) p)) {
            p = (const uint64_t *) (((uint64_t) p + PAGE_SIZE) & ~(uint64_t) (PAGE_SIZE - 1));
            continue;
        }
        uint64_t val = *p;
        if (val != 0 && !in_entries_range(p)) {
            for (int i = 0; i < KMEMLEAK_MAX; i++) {
                if (g_entries[i].used && !g_entries[i].reachable &&
                    (uint64_t) g_entries[i].ptr == val) {
                    g_entries[i].reachable = 1;
                }
            }
            if (val < g_max_phys) {
                try_mark_page(val);
            } else if (val >= g_hhdm_offset) {
                try_mark_page(virt_to_phys((void *) val));
            } else if (val >= HEAP_START && val < HEAP_MAX) {
                try_mark_page(vmm_virt_to_phys(&g_kernel_space, val));
            } else if (is_kernel_addr((void *) val)) {
                try_mark_page(vmm_virt_to_phys(&g_kernel_space, val));
            }
        }
        p++;
    }
}

static void scan_used_block(void *data, uint64_t size, void *user) {
    (void) user;
    scan_region((const uint8_t *) data, (const uint8_t *) data + size);
}

static void walk_page_table(vmm_space_t *sp, int start, int end, int mark_leaf) {
    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    for (int i = start; i < end; i++) {
        if (!(pml4[i] & VMM_PRESENT)) continue;
        try_mark_page(pml4[i] & PTE_ADDR_MASK);

        uint64_t *pdpt = (uint64_t *) phys_to_virt(pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_PRESENT)) continue;
            try_mark_page(pdpt[j] & PTE_ADDR_MASK);
            if (pdpt[j] & (1ULL << 7)) {
                if (mark_leaf) try_mark_page(pdpt[j] & PTE_ADDR_MASK);
                continue;
            }

            uint64_t *pd = (uint64_t *) phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT)) continue;
                try_mark_page(pd[k] & PTE_ADDR_MASK);
                if (pd[k] & (1ULL << 7)) {
                    if (mark_leaf) try_mark_page(pd[k] & PTE_ADDR_MASK);
                    continue;
                }

                if (mark_leaf) {
                    uint64_t *pt = (uint64_t *) phys_to_virt(pd[k] & PTE_ADDR_MASK);
                    for (int l = 0; l < 512; l++) {
                        if (!(pt[l] & VMM_PRESENT)) continue;
                        try_mark_page(pt[l] & PTE_ADDR_MASK);
                    }
                }
            }
        }
    }
}

static int do_scan(void) {
    g_max_phys = pmm_total_pages() << PAGE_SHIFT;

    for (int i = 0; i < KMEMLEAK_MAX; i++) g_entries[i].reachable = 0;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) g_pages[i].reachable = g_pages[i].perm;

    scan_region(__data_start, __data_end);
    scan_region(__bss_start, __bss_end);
    scan_region((const uint8_t *) g_proctable, (const uint8_t *) (g_proctable + PROC_MAX));

    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &g_proctable[i];
        if (p->state == PROC_UNUSED || !p->kstack_guard) continue;
        scan_region(p->kstack, p->kstack + KSTACK_SIZE);
        for (uint64_t pg = 0; pg < KSTACK_PAGES; pg++) {
            uint64_t va = p->kstack_guard + PAGE_SIZE + pg * PAGE_SIZE;
            uint64_t phys = vmm_virt_to_phys(&g_kernel_space, va);
            if (phys) try_mark_page(phys);
        }
    }

    uint64_t hbrk = heap_brk();
    for (uint64_t va = HEAP_START; va < hbrk; va += PAGE_SIZE) {
        uint64_t phys = vmm_virt_to_phys(&g_kernel_space, va);
        if (phys) try_mark_page(phys);
    }

    heap_walk_used(scan_used_block, NULL);

    walk_page_table(&g_kernel_space, 0, 510, 0);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &g_proctable[i];
        if (p->state == PROC_UNUSED) continue;
        vmm_space_t *sp = p->space;
        if (!sp || sp == &g_kernel_space) continue;
        walk_page_table(sp, 0, 256, 1);
    }

    int leaked = 0;
    for (int i = 0; i < KMEMLEAK_MAX; i++) {
        if (g_entries[i].used && !g_entries[i].reachable) leaked++;
    }
    return leaked;
}

int kmemleak_report(char *buf, uint64_t bufsz) {
    do_scan();
    uint64_t pos = 0;
    int nleak = 0;
    int n;

    for (int i = 0; i < KMEMLEAK_MAX; i++) {
        if (!g_entries[i].used || g_entries[i].reachable) continue;
        nleak++;
        n = snprintf(buf + pos, bufsz - pos, "  %3d. %p  size=%lu", nleak, g_entries[i].ptr,
                     g_entries[i].size);
        if (n < 0 || (uint64_t) n >= bufsz - pos) break;
        pos += (uint64_t) n;

        for (int j = 0; j < g_entries[i].bt_depth; j++) {
            if (g_entries[i].backtrace[j] == 0) break;
            const char *sym = kallsyms_lookup(g_entries[i].backtrace[j]);
            n = snprintf(buf + pos, bufsz - pos, "  %s", sym);
            if (n < 0 || (uint64_t) n >= bufsz - pos) break;
            pos += (uint64_t) n;
        }

        n = snprintf(buf + pos, bufsz - pos, "\n");
        if (n < 0 || (uint64_t) n >= bufsz - pos) break;
        pos += (uint64_t) n;
    }

    int page_leaks = 0;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++) {
        if (!g_pages[i].used || g_pages[i].reachable) continue;
        page_leaks++;
        n = snprintf(buf + pos, bufsz - pos, "  page %3d. phys=0x%lx", page_leaks, g_pages[i].phys);
        if (n < 0 || (uint64_t) n >= bufsz - pos) break;
        pos += (uint64_t) n;

        for (int j = 0; j < g_pages[i].bt_depth; j++) {
            if (g_pages[i].backtrace[j] == 0) break;
            const char *sym = kallsyms_lookup(g_pages[i].backtrace[j]);
            n = snprintf(buf + pos, bufsz - pos, "  %s", sym);
            if (n < 0 || (uint64_t) n >= bufsz - pos) break;
            pos += (uint64_t) n;
        }

        n = snprintf(buf + pos, bufsz - pos, "\n");
        if (n < 0 || (uint64_t) n >= bufsz - pos) break;
        pos += (uint64_t) n;
    }

    int tracked = 0;
    for (int i = 0; i < KMEMLEAK_MAX; i++)
        if (g_entries[i].used) tracked++;
    int ptracked = 0;
    for (int i = 0; i < KMEMLEAK_PAGE_MAX; i++)
        if (g_pages[i].used) ptracked++;

    int total = nleak + page_leaks;
    n = snprintf(buf + pos, bufsz - pos,
                 "KMEMLEAK: %d leak(s) (heap=%d pages=%d tracked=%d ptracked=%d)\n", total, nleak,
                 page_leaks, tracked, ptracked);
    if (n > 0 && (uint64_t) n < bufsz - pos) pos += (uint64_t) n;

    return total;
}

int kmemleak_checkpoint(void) {
    int leaked = do_scan();
    for (int i = 0; i < KMEMLEAK_MAX; i++) g_entries[i].used = 0;
    return leaked;
}

void kmemleak_clear(void) {
    for (int i = 0; i < KMEMLEAK_MAX; i++) g_entries[i].used = 0;
}

#endif /* CONFIG_KMEMLEAK */
