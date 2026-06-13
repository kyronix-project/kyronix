#include "pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/log.h"
#include "lib/string.h"

uint64_t g_hhdm_offset;

typedef struct {
    uint64_t* words; /* bit=1 -> page FREE, bit=0 -> page USED */
    uint64_t total_pages;
    uint64_t free_pages;
} pmm_t;

static pmm_t g_pmm;

static inline void bitmap_set_free(uint64_t page)
{
    g_pmm.words[page >> 6] |= (1ULL << (page & 63));
}

static inline void bitmap_set_used(uint64_t page)
{
    g_pmm.words[page >> 6] &= ~(1ULL << (page & 63));
}

static inline int bitmap_is_free(uint64_t page)
{
    return !!(g_pmm.words[page >> 6] & (1ULL << (page & 63)));
}

void pmm_init(struct limine_memmap_response* mmap, uint64_t hhdm_offset)
{
    g_hhdm_offset = hhdm_offset;

    uint64_t highest = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t top = e->base + e->length;
        if (top > highest)
            highest = top;
    }

    g_pmm.total_pages = highest >> PAGE_SHIFT;
    g_pmm.free_pages = 0;

    uint64_t bitmap_bytes = PAGE_ALIGN_UP((g_pmm.total_pages + 7) / 8);
    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        if (e->length >= bitmap_bytes) {
            bitmap_phys = e->base;
            break;
        }
    }

    g_pmm.words = (uint64_t*) (bitmap_phys + hhdm_offset);
    memset(g_pmm.words, 0, bitmap_bytes);

    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry* e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;

        uint64_t first = e->base >> PAGE_SHIFT;
        uint64_t count = e->length >> PAGE_SHIFT;
        for (uint64_t p = first; p < first + count; p++) {
            bitmap_set_free(p);
            g_pmm.free_pages++;
        }
    }

    uint64_t bm_first = bitmap_phys >> PAGE_SHIFT;
    uint64_t bm_count = bitmap_bytes >> PAGE_SHIFT;
    for (uint64_t p = bm_first; p < bm_first + bm_count; p++) {
        if (bitmap_is_free(p)) {
            bitmap_set_used(p);
            g_pmm.free_pages--;
        }
    }

    log_info("PMM: %lu MiB free  (%lu pages, %lu total)",
             (uint64_t) (g_pmm.free_pages * PAGE_SIZE) >> 20, g_pmm.free_pages, g_pmm.total_pages);
    log_info("PMM: bitmap  phys=0x%016lx  size=%lu KiB", bitmap_phys, bitmap_bytes >> 10);
}

void* pmm_alloc(void)
{
    uint64_t nwords = (g_pmm.total_pages + 63) >> 6;

    for (uint64_t wi = 0; wi < nwords; wi++) {
        if (g_pmm.words[wi] == 0)
            continue; /* no free page in word */

        uint32_t bit = (uint32_t) __builtin_ctzll(g_pmm.words[wi]);
        uint64_t page = (wi << 6) | bit;

        if (page >= g_pmm.total_pages)
            break; /* stray bit at end */

        g_pmm.words[wi] &= ~(1ULL << bit); /* mark used */
        g_pmm.free_pages--;
        return (void*) (page << PAGE_SHIFT);
    }
    return NULL; /* out of phys memory */
}

void* pmm_alloc_zeroed(void)
{
    void* phys = pmm_alloc();
    if (!phys)
        return NULL;
    memset(phys_to_virt((uint64_t) phys), 0, PAGE_SIZE);
    return phys;
}

void pmm_free(void* phys)
{
    uint64_t page = (uint64_t) phys >> PAGE_SHIFT;
    if (!phys || page >= g_pmm.total_pages)
        return;
    bitmap_set_free(page);
    g_pmm.free_pages++;
}

uint64_t pmm_free_pages(void)
{
    return g_pmm.free_pages;
}
uint64_t pmm_total_pages(void)
{
    return g_pmm.total_pages;
}
