#include "heap.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "pmm.h"
#include "vmm.h"

typedef struct block_hdr {
    uint64_t size;
    uint64_t free;
    struct block_hdr* prev;
    struct block_hdr* next;
} block_hdr_t;

#define HDR_SIZE ((uint64_t) sizeof(block_hdr_t))
#define MIN_SPLIT (HDR_SIZE + 16)
#define GROW_PAGES 16
#define GROW_BYTES ((uint64_t) (GROW_PAGES) * PAGE_SIZE)

static block_hdr_t* g_head = NULL;
static uint64_t g_brk = HEAP_START;

static block_hdr_t* heap_grow(void)
{
    if (g_brk + GROW_BYTES > HEAP_MAX)
        return NULL;

    for (uint64_t va = g_brk; va < g_brk + GROW_BYTES; va += PAGE_SIZE) {
        void* phys = pmm_alloc();
        if (!phys)
            return NULL;
        if (vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0) {
            pmm_free(phys);
            return NULL;
        }
    }

    block_hdr_t* blk = (block_hdr_t*) g_brk;
    g_brk += GROW_BYTES;

    if (g_head) {
        block_hdr_t* last = g_head;
        while (last->next)
            last = last->next;

        if (last->free) {
            last->size += GROW_BYTES;
            return last;
        }
        blk->size = GROW_BYTES - HDR_SIZE;
        blk->free = 1;
        blk->prev = last;
        blk->next = NULL;
        last->next = blk;
    } else {
        blk->size = GROW_BYTES - HDR_SIZE;
        blk->free = 1;
        blk->prev = NULL;
        blk->next = NULL;
        g_head = blk;
    }
    return blk;
}

void heap_init(void)
{
    heap_grow();
    log_info("Heap: base=0x%016lx  initial=%lu KiB", (uint64_t) HEAP_START,
             (uint64_t) (GROW_BYTES >> 10));
}

void* kmalloc(uint64_t size)
{
    if (!size)
        return NULL;

    size = (size + 15) & ~15ULL;

    block_hdr_t* blk = g_head;
    while (blk) {
        if (blk->free && blk->size >= size)
            break;
        blk = blk->next;
    }

    while (!blk || !blk->free || blk->size < size) {
        blk = heap_grow();
        if (!blk)
            return NULL;
        if (blk->size >= size)
            break;
    }

    if (blk->size >= size + MIN_SPLIT) {
        block_hdr_t* tail = (block_hdr_t*) ((uint8_t*) blk + HDR_SIZE + size);
        tail->size = blk->size - size - HDR_SIZE;
        tail->free = 1;
        tail->prev = blk;
        tail->next = blk->next;
        if (blk->next)
            blk->next->prev = tail;
        blk->next = tail;
        blk->size = size;
    }

    blk->free = 0;
    return (uint8_t*) blk + HDR_SIZE;
}

void kfree(void* ptr)
{
    if (!ptr)
        return;

    block_hdr_t* blk = (block_hdr_t*) ((uint8_t*) ptr - HDR_SIZE);
    if (blk->free) /* double free, very cool thing!!! */
    {
        return;
    }
    blk->free = 1;

    if (blk->next && blk->next->free) {
        blk->size += HDR_SIZE + blk->next->size;
        blk->next = blk->next->next;
        if (blk->next)
            blk->next->prev = blk;
    }

    if (blk->prev && blk->prev->free) {
        blk->prev->size += HDR_SIZE + blk->size;
        blk->prev->next = blk->next;
        if (blk->next)
            blk->next->prev = blk->prev;
    }
}

void* kcalloc(uint64_t nmemb, uint64_t size)
{
    uint64_t total = nmemb * size;
    void* ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void* krealloc(void* ptr, uint64_t new_size)
{
    if (!ptr)
        return kmalloc(new_size);
    if (!new_size) {
        kfree(ptr);
        return NULL;
    }

    block_hdr_t* blk = (block_hdr_t*) ((uint8_t*) ptr - HDR_SIZE);
    uint64_t aligned = (new_size + 15) & ~15ULL;

    if (blk->size >= aligned)
        return ptr;

    void* new_ptr = kmalloc(new_size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, blk->size);
    kfree(ptr);
    return new_ptr;
}

void heap_stats(void)
{
    uint64_t free_bytes = 0, used_bytes = 0, nblocks = 0;
    block_hdr_t* b = g_head;
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
}
