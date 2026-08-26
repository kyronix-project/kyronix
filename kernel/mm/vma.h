#pragma once
#include "vmm.h"

#define VMA_MAP_SHARED 0x01u
#include <stdbool.h>
#include <stdint.h>

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

void vma_reset(vmm_space_t *sp);
void vma_copy(vmm_space_t *dst, const vmm_space_t *src);
bool vma_conflicts(vmm_space_t *sp, uint64_t start, uint64_t len);
int vma_add(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t prot, uint32_t map_flags,
            bool free_on_unmap);
bool vma_page_mapped(vmm_space_t *sp, uint64_t addr);
int vma_used_count(vmm_space_t *sp);
bool vma_lookup(vmm_space_t *sp, uint64_t addr, uint64_t *start, uint64_t *end, uint32_t *prot);
bool vma_range_ok(vmm_space_t *sp, uint64_t start, uint64_t len);
bool vma_page_owned(vmm_space_t *sp, uint64_t addr);
bool vma_range_info(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t *prot,
                    uint32_t *map_flags, bool *free_on_unmap);
bool vma_page_fault_allowed(vmm_space_t *sp, uint64_t addr, bool write, bool exec);
uint64_t vma_page_flags(vmm_space_t *sp, uint64_t addr);
int vma_remove(vmm_space_t *sp, uint64_t start, uint64_t len);
int vma_remove_overlaps(vmm_space_t *sp, uint64_t start, uint64_t len);
int vma_protect(vmm_space_t *sp, uint64_t start, uint64_t len, uint32_t prot);
