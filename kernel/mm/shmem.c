#include "shmem.h"
#include "../lib/string.h"
#include "heap.h"
#include "pmm.h"

#define EINVAL 22
#define ENOMEM 12
#define EPERM 1
#define EFBIG 27

static uint64_t pages_for(uint64_t size) { return (size + PAGE_SIZE - 1) / PAGE_SIZE; }

shmem_obj_t *shmem_create(void) {
    shmem_obj_t *o = (shmem_obj_t *) kcalloc(1, sizeof(shmem_obj_t));
    if (!o) return NULL;
    o->refcnt = 1;
    return o;
}

void shmem_ref(shmem_obj_t *o) {
    if (o) o->refcnt++;
}

void shmem_unref(shmem_obj_t *o) {
    if (!o) return;
    if (o->refcnt && --o->refcnt) return;
    for (uint64_t i = 0; i < o->npages; i++)
        if (o->pages[i]) pmm_free((void *) o->pages[i]);
    kfree(o->pages);
    kfree(o);
}

static int shmem_grow_array(shmem_obj_t *o, uint64_t want) {
    if (want <= o->npages) return 0;
    uint64_t *pages = (uint64_t *) krealloc(o->pages, want * sizeof(uint64_t));
    if (!pages) return -ENOMEM;
    for (uint64_t i = o->npages; i < want; i++) pages[i] = 0;
    o->pages = pages;
    o->npages = want;
    return 0;
}

int shmem_resize(shmem_obj_t *o, uint64_t size) {
    if (!o) return -EINVAL;
    if (size == o->size) return 0;
    if (size > o->size && (o->seals & F_SEAL_GROW)) return -EPERM;
    if (size < o->size && (o->seals & F_SEAL_SHRINK)) return -EPERM;
    if (pages_for(size) > SHMEM_MAX_PAGES) return -EFBIG;

    if (size > o->size) {
        uint64_t want = pages_for(size);
        int rc = shmem_grow_array(o, want);
        if (rc < 0) return rc;
        for (uint64_t i = 0; i < want; i++) {
            if (o->pages[i]) continue;
            void *p = pmm_alloc_zeroed();
            if (!p) return -ENOMEM;
            o->pages[i] = (uint64_t) p;
        }
        // zero the tail of the last page that becomes visible again
        uint64_t tail = o->size & (PAGE_SIZE - 1);
        if (tail) {
            uint8_t *page = (uint8_t *) phys_to_virt(o->pages[o->size / PAGE_SIZE]);
            memset(page + tail, 0, PAGE_SIZE - tail);
        }
    } else {
        // keep frames that are still inside the new size, drop the rest
        uint64_t keep = pages_for(size);
        for (uint64_t i = keep; i < o->npages; i++) {
            if (!o->pages[i]) continue;
            pmm_free((void *) o->pages[i]);
            o->pages[i] = 0;
        }
    }
    o->size = size;
    return 0;
}

uint64_t shmem_page_phys(shmem_obj_t *o, uint64_t off) {
    if (!o) return 0;
    uint64_t idx = off / PAGE_SIZE;
    if (idx >= SHMEM_MAX_PAGES) return 0;
    if (shmem_grow_array(o, idx + 1) < 0) return 0;
    if (!o->pages[idx]) {
        void *p = pmm_alloc_zeroed();
        if (!p) return 0;
        o->pages[idx] = (uint64_t) p;
    }
    return o->pages[idx];
}

int64_t shmem_read(shmem_obj_t *o, void *dst, uint64_t off, uint64_t len) {
    if (!o || !dst) return -EINVAL;
    if (off >= o->size) return 0;
    uint64_t avail = o->size - off;
    if (len > avail) len = avail;

    uint8_t *out = (uint8_t *) dst;
    uint64_t done = 0;
    while (done < len) {
        uint64_t phys = shmem_page_phys(o, off + done);
        if (!phys) return done ? (int64_t) done : -ENOMEM;
        uint64_t in_page = (off + done) & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - in_page;
        if (chunk > len - done) chunk = len - done;
        const uint8_t *src = (const uint8_t *) phys_to_virt(phys) + in_page;
        memcpy(out + done, src, chunk);
        done += chunk;
    }
    return (int64_t) done;
}

int64_t shmem_write(shmem_obj_t *o, const void *src, uint64_t off, uint64_t len) {
    if (!o || !src) return -EINVAL;
    if (o->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE)) return -EPERM;
    if (len == 0) return 0;
    if (off > UINT64_MAX - len) return -EFBIG;
    if (off + len > o->size) {
        int rc = shmem_resize(o, off + len);
        if (rc < 0) return rc;
    }

    const uint8_t *in = (const uint8_t *) src;
    uint64_t done = 0;
    while (done < len) {
        uint64_t phys = shmem_page_phys(o, off + done);
        if (!phys) return done ? (int64_t) done : -ENOMEM;
        uint64_t in_page = (off + done) & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - in_page;
        if (chunk > len - done) chunk = len - done;
        uint8_t *dst = (uint8_t *) phys_to_virt(phys) + in_page;
        memcpy(dst, in + done, chunk);
        done += chunk;
    }
    return (int64_t) done;
}

int shmem_add_seals(shmem_obj_t *o, uint32_t seals) {
    if (!o) return -EINVAL;
    if (seals & ~(F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))
        return -EINVAL;
    if (o->seals & F_SEAL_SEAL) return -EPERM;
    o->seals |= seals;
    return 0;
}

int shmem_get_seals(shmem_obj_t *o) { return o ? (int) o->seals : -EINVAL; }
