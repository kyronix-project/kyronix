#pragma once
#include "boot/limine.h"
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define PAGE_MASK (~(PAGE_SIZE - 1))

#define PAGE_ALIGN_UP(x) (((uint64_t) (x) + PAGE_SIZE - 1) & PAGE_MASK)
#define PAGE_ALIGN_DOWN(x) ((uint64_t) (x) & PAGE_MASK)

extern uint64_t g_hhdm_offset;

static inline void *phys_to_virt(uint64_t phys) { return (void *) (phys + g_hhdm_offset); }

static inline uint64_t virt_to_phys(const void *virt) { return (uint64_t) virt - g_hhdm_offset; }

void pmm_init(struct limine_memmap_response *mmap, uint64_t hhdm_offset, uint64_t kernel_end_phys);

void *pmm_alloc(void);
void *pmm_alloc_zeroed(void);
void *pmm_alloc_contiguous(uint64_t n_pages); /* n physically-consecutive pages */
void pmm_free_contiguous(void *phys, uint64_t n_pages);
void pmm_free(void *phys);
void pmm_retain(void *phys);

uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);
uint64_t pmm_usable_pages(void);
uint64_t pmm_alloc_total(void);
uint64_t pmm_free_total(void);
