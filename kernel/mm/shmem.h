#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SHMEM_MAX_PAGES (256ULL * 1024ULL)

#define F_SEAL_SEAL 0x0001U
#define F_SEAL_SHRINK 0x0002U
#define F_SEAL_GROW 0x0004U
#define F_SEAL_WRITE 0x0008U
#define F_SEAL_FUTURE_WRITE 0x0010U

typedef struct shmem_obj {
    uint64_t *pages; // physical frame addresses
    uint64_t npages;
    uint64_t size; // logical size in bytes
    uint32_t refcnt;
    uint32_t seals;
} shmem_obj_t;

shmem_obj_t *shmem_create(void);
void shmem_ref(shmem_obj_t *o);
void shmem_unref(shmem_obj_t *o);

int shmem_resize(shmem_obj_t *o, uint64_t size);

uint64_t shmem_page_phys(shmem_obj_t *o, uint64_t off);

int64_t shmem_read(shmem_obj_t *o, void *dst, uint64_t off, uint64_t len);
int64_t shmem_write(shmem_obj_t *o, const void *src, uint64_t off, uint64_t len);

int shmem_add_seals(shmem_obj_t *o, uint32_t seals);
int shmem_get_seals(shmem_obj_t *o);
