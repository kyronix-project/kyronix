#include "heap.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "arch/x86_64/percpu.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "pmm.h"
#include "vmm.h"
#ifdef CONFIG_KMEMLEAK
#include "kmemleak.h"
#endif

typedef struct block_hdr {
    uint64_t size;
    uint64_t free;
    struct block_hdr *prev;
    struct block_hdr *next;
    struct block_hdr *free_prev;
    struct block_hdr *free_next;
} block_hdr_t;

#define HDR_SIZE ((uint64_t) sizeof(block_hdr_t))
#define MIN_SPLIT (HDR_SIZE + 16)
#define GROW_PAGES 16
#define GROW_BYTES ((uint64_t) (GROW_PAGES) * PAGE_SIZE)
#define HEAP_CAPACITY ((uint64_t) (HEAP_MAX - HEAP_START))
#define HEAP_BIN_COUNT 32
#define SMALL_CLASS_COUNT 16
#define SMALL_CACHE_LIMIT 32

typedef struct small_free {
    struct small_free *next;
} small_free_t;

typedef struct __attribute__((aligned(64))) {
    small_free_t *head[SMALL_CLASS_COUNT];
    uint8_t count[SMALL_CLASS_COUNT];
} small_cache_t;

static spinlock_t g_heap_lock = SPINLOCK_INIT;
static block_hdr_t *g_head = NULL;
static block_hdr_t *g_tail = NULL;
static block_hdr_t *g_free_bins[HEAP_BIN_COUNT];
static uint64_t g_brk = HEAP_START;
static uint64_t g_kmalloc_total = 0;
static uint64_t g_kfree_total = 0;
static small_cache_t g_small_cache[MAX_CPUS];

static unsigned bin_for_size(uint64_t size) {
    uint64_t units = (size + 15) >> 4;
    unsigned bin = 0;
    while (units > 1 && bin + 1 < HEAP_BIN_COUNT) {
        units = (units + 1) >> 1;
        bin++;
    }
    return bin;
}

static void free_remove(block_hdr_t *blk) {
    unsigned bin = bin_for_size(blk->size);
    if (blk->free_prev)
        blk->free_prev->free_next = blk->free_next;
    else
        g_free_bins[bin] = blk->free_next;
    if (blk->free_next) blk->free_next->free_prev = blk->free_prev;
    blk->free_prev = NULL;
    blk->free_next = NULL;
}

static void free_insert(block_hdr_t *blk) {
    unsigned bin = bin_for_size(blk->size);
    blk->free_prev = NULL;
    blk->free_next = g_free_bins[bin];
    if (blk->free_next) blk->free_next->free_prev = blk;
    g_free_bins[bin] = blk;
}

static block_hdr_t *find_fit(uint64_t size) {
    for (unsigned bin = bin_for_size(size); bin < HEAP_BIN_COUNT; bin++) {
        for (block_hdr_t *blk = g_free_bins[bin]; blk; blk = blk->free_next)
            if (blk->size >= size) return blk;
    }
    return NULL;
}

static block_hdr_t *heap_grow(uint64_t min_payload) {
    if (min_payload > HEAP_CAPACITY - HDR_SIZE) return NULL;
    uint64_t need = min_payload + HDR_SIZE;
    if (need < GROW_BYTES) need = GROW_BYTES;
    if (need > UINT64_MAX - (PAGE_SIZE - 1)) return NULL;
    need = (need + (PAGE_SIZE - 1)) & ~(uint64_t) (PAGE_SIZE - 1);
    uint64_t npages = need / PAGE_SIZE;

    if (g_brk > HEAP_MAX || need > HEAP_MAX - g_brk) return NULL;

    for (uint64_t i = 0; i < npages; i++) {
        uint64_t va = g_brk + i * PAGE_SIZE;
        void *phys = pmm_alloc();
        if (!phys || vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0) {
            if (phys) pmm_free(phys);
            for (uint64_t j = 0; j < i; j++) {
                uint64_t old_phys =
                    vmm_virt_to_phys(&g_kernel_space, g_brk + j * PAGE_SIZE);
                vmm_unmap(&g_kernel_space, g_brk + j * PAGE_SIZE);
                if (old_phys) pmm_free((void *) old_phys);
            }
            return NULL;
        }
    }

    block_hdr_t *blk = (block_hdr_t *) g_brk;
    g_brk += need;

    if (g_head) {
        if (g_tail->free == 1) {
            free_remove(g_tail);
            g_tail->size += need;
            free_insert(g_tail);
            return g_tail;
        }
        blk->size = need - HDR_SIZE;
        blk->free = 1;
        blk->prev = g_tail;
        blk->next = NULL;
        blk->free_prev = NULL;
        blk->free_next = NULL;
        g_tail->next = blk;
        g_tail = blk;
    } else {
        blk->size = need - HDR_SIZE;
        blk->free = 1;
        blk->prev = NULL;
        blk->next = NULL;
        blk->free_prev = NULL;
        blk->free_next = NULL;
        g_head = blk;
        g_tail = blk;
    }
    free_insert(blk);
    return blk;
}

void heap_init(void) {
    heap_grow(0);
    log_info("Heap: base=0x%016lx  initial=%lu KiB", (uint64_t) HEAP_START,
             (uint64_t) (GROW_BYTES >> 10));
}

void *kmalloc(uint64_t size) {
    if (!size) return NULL;
    if (size > HEAP_CAPACITY || size > UINT64_MAX - 15) return NULL;

    size = (size + 15) & ~15ULL;

    if (size <= SMALL_CLASS_COUNT * 16u) {
        uint64_t flags = irq_save();
        small_cache_t *cache = &g_small_cache[this_cpu_id()];
        unsigned cls = (unsigned) (size / 16u - 1u);
        small_free_t *item = cache->head[cls];
        if (item) {
            cache->head[cls] = item->next;
            cache->count[cls]--;
            block_hdr_t *blk = (block_hdr_t *) ((uint8_t *) item - HDR_SIZE);
            __atomic_store_n(&blk->free, 0, __ATOMIC_RELEASE);
            irq_restore(flags);
            __atomic_fetch_add(&g_kmalloc_total, size, __ATOMIC_RELAXED);
#ifdef CONFIG_KMEMLEAK
            kmemleak_track(item, size);
#endif
            return item;
        }
        irq_restore(flags);
    }

    uint64_t flags = irq_save();
    spin_lock(&g_heap_lock);

    block_hdr_t *blk = find_fit(size);

    while (!blk) {
        blk = heap_grow(size);
        if (!blk) {
            spin_unlock(&g_heap_lock);
            irq_restore(flags);
            return NULL;
        }
    }
    free_remove(blk);

    if (blk->size >= size + MIN_SPLIT) {
        block_hdr_t *tail = (block_hdr_t *) ((uint8_t *) blk + HDR_SIZE + size);
        tail->size = blk->size - size - HDR_SIZE;
        tail->free = 1;
        tail->prev = blk;
        tail->next = blk->next;
        tail->free_prev = NULL;
        tail->free_next = NULL;
        if (blk->next) blk->next->prev = tail;
        if (g_tail == blk) g_tail = tail;
        blk->next = tail;
        blk->size = size;
        free_insert(tail);
    }

    blk->free = 0;
    blk->free_prev = NULL;
    blk->free_next = NULL;
    __atomic_fetch_add(&g_kmalloc_total, blk->size, __ATOMIC_RELAXED);
    spin_unlock(&g_heap_lock);
    irq_restore(flags);
#ifdef CONFIG_KMEMLEAK
    kmemleak_track((uint8_t *) blk + HDR_SIZE, blk->size);
#endif
    return (uint8_t *) blk + HDR_SIZE;
}

void kfree(void *ptr) {
    if (!ptr) return;

#ifdef CONFIG_KMEMLEAK
    kmemleak_untrack(ptr);
#endif

    block_hdr_t *blk = (block_hdr_t *) ((uint8_t *) ptr - HDR_SIZE);
    /* State 2 claims the block without exposing it to coalescing before it is
       actually linked into a cache or a global free bin. */
    if (!__sync_bool_compare_and_swap(&blk->free, 0, 2)) return;
    if (blk->size && blk->size <= SMALL_CLASS_COUNT * 16u &&
        !(blk->size & 15u)) {
        uint64_t flags = irq_save();
        small_cache_t *cache = &g_small_cache[this_cpu_id()];
        unsigned cls = (unsigned) (blk->size / 16u - 1u);
        if (cache->count[cls] < SMALL_CACHE_LIMIT) {
            small_free_t *item = (small_free_t *) ptr;
            item->next = cache->head[cls];
            cache->head[cls] = item;
            cache->count[cls]++;
            /* Cached blocks are free, but are not members of a global bin and
               therefore must never be coalesced by another free operation. */
            __atomic_store_n(&blk->free, 3, __ATOMIC_RELEASE);
            irq_restore(flags);
            __atomic_fetch_add(&g_kfree_total, blk->size, __ATOMIC_RELAXED);
            return;
        }
        irq_restore(flags);
    }

    uint64_t flags = irq_save();
    spin_lock(&g_heap_lock);

    __atomic_fetch_add(&g_kfree_total, blk->size, __ATOMIC_RELAXED);

    if (blk->next && blk->next->free == 1) {
        block_hdr_t *next = blk->next;
        free_remove(next);
        blk->size += HDR_SIZE + blk->next->size;
        blk->next = blk->next->next;
        if (blk->next) blk->next->prev = blk;
        if (g_tail == next) g_tail = blk;
    }

    if (blk->prev && blk->prev->free == 1) {
        block_hdr_t *prev = blk->prev;
        free_remove(prev);
        prev->size += HDR_SIZE + blk->size;
        prev->next = blk->next;
        if (blk->next) blk->next->prev = prev;
        if (g_tail == blk) g_tail = prev;
        blk = prev;
    }
    blk->free = 1;
    free_insert(blk);

    spin_unlock(&g_heap_lock);
    irq_restore(flags);
}

void *kcalloc(uint64_t nmemb, uint64_t size) {
    if (size && nmemb > UINT64_MAX / size) return NULL;
    uint64_t total = nmemb * size;
    void *ptr = kmalloc(total);
    if (ptr) memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, uint64_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (!new_size) {
        kfree(ptr);
        return NULL;
    }

    block_hdr_t *blk = (block_hdr_t *) ((uint8_t *) ptr - HDR_SIZE);
    if (new_size > HEAP_CAPACITY || new_size > UINT64_MAX - 15) return NULL;
    uint64_t aligned = (new_size + 15) & ~15ULL;

    if (blk->size >= aligned) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, blk->size);
    kfree(ptr);
    return new_ptr;
}

int64_t heap_alloc_delta(void) { return (int64_t) (g_kmalloc_total - g_kfree_total); }

uint64_t heap_brk(void) { return g_brk; }

void heap_walk_used(void (*callback)(void *data, uint64_t size, void *user), void *user) {
    uint64_t flags = irq_save();
    spin_lock(&g_heap_lock);
    block_hdr_t *b = g_head;
    while (b) {
        if (!b->free) callback((uint8_t *) b + HDR_SIZE, b->size, user);
        b = b->next;
    }
    spin_unlock(&g_heap_lock);
    irq_restore(flags);
}

void heap_stats(void) {
    uint64_t flags = irq_save();
    spin_lock(&g_heap_lock);
    uint64_t free_bytes = 0, used_bytes = 0, nblocks = 0;
    block_hdr_t *b = g_head;
    while (b) {
        nblocks++;
        if (b->free)
            free_bytes += b->size;
        else
            used_bytes += b->size;
        b = b->next;
    }
    log_info("Heap: %lu blocks  used=%lu KiB  free=%lu KiB", nblocks, used_bytes >> 10,
             free_bytes >> 10);
    spin_unlock(&g_heap_lock);
    irq_restore(flags);
}
