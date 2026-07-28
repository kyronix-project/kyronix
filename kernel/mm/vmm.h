#pragma once
#include <stdbool.h>
#include <stdint.h>

#define VMM_PRESENT (1ULL << 0)
#define VMM_WRITE (1ULL << 1)
#define VMM_USER (1ULL << 2)
#define VMM_NX (1ULL << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL /* bits [51:12] */
#define PTE_FLAGS_MASK (VMM_NX | 0x0000000000000FFFULL)

#define VMM_PCD (1ULL << 4)
#define VMM_COW (1ULL << 9) /* software bit: write fault must copy the page */

#define VMM_KCODE (VMM_PRESENT)
#define VMM_KDATA (VMM_PRESENT | VMM_WRITE | VMM_NX)
#define VMM_UCODE (VMM_PRESENT | VMM_USER)
#define VMM_UDATA (VMM_PRESENT | VMM_WRITE | VMM_USER | VMM_NX)

#define VMM_VMA_MAX 2048

/* Top of the user address half; everything >= this is the shared kernel half. */
#ifndef USER_LIMIT
#define USER_LIMIT 0x800000000000ULL
#endif

typedef struct {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t map_flags;
    uint8_t used;
    uint8_t free_on_unmap;
} vmm_vma_t;

typedef struct {
    uint64_t pml4_phys;
    uint64_t kernel_map_generation;
    vmm_vma_t vmas[VMM_VMA_MAX];
} vmm_space_t;

extern vmm_space_t g_kernel_space;

void vmm_init(void);
int vmm_map(vmm_space_t *sp, uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(vmm_space_t *sp, uint64_t virt);
uint64_t vmm_virt_to_phys(vmm_space_t *sp, uint64_t virt);
bool vmm_user_range_ok(vmm_space_t *sp, uint64_t virt, uint64_t len, bool write);
bool vmm_user_range_fault_in(vmm_space_t *sp, uint64_t virt, uint64_t len, bool write);
int vmm_protect(vmm_space_t *sp, uint64_t virt, uint64_t flags);
vmm_space_t *vmm_space_new(void);
void vmm_space_free(vmm_space_t *sp);
void vmm_switch(vmm_space_t *sp);
int vmm_fork_user(vmm_space_t *dst, vmm_space_t *src);
int vmm_fork_user_cow(vmm_space_t *dst, vmm_space_t *src);
int vmm_handle_cow_fault(vmm_space_t *sp, uint64_t virt);
int vmm_phantom_relax_page(vmm_space_t *sp, uint64_t virt, bool write, bool execute);
