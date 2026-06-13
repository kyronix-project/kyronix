#pragma once
#include "vmm.h"
#include <stdbool.h>
#include <stdint.h>

void vma_reset(vmm_space_t* sp);
void vma_copy(vmm_space_t* dst, const vmm_space_t* src);
bool vma_conflicts(vmm_space_t* sp, uint64_t start, uint64_t len);
int vma_add(vmm_space_t* sp, uint64_t start, uint64_t len, uint32_t prot, uint32_t map_flags,
            bool free_on_unmap);
bool vma_page_mapped(vmm_space_t* sp, uint64_t addr);
bool vma_range_ok(vmm_space_t* sp, uint64_t start, uint64_t len);
bool vma_page_owned(vmm_space_t* sp, uint64_t addr);
bool vma_page_fault_allowed(vmm_space_t* sp, uint64_t addr, bool write, bool exec);
uint64_t vma_page_flags(vmm_space_t* sp, uint64_t addr);
int vma_remove(vmm_space_t* sp, uint64_t start, uint64_t len);
int vma_remove_overlaps(vmm_space_t* sp, uint64_t start, uint64_t len);
int vma_protect(vmm_space_t* sp, uint64_t start, uint64_t len, uint32_t prot);
