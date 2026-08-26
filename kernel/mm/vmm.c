#include "vmm.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "lib/log.h"
#include "lib/string.h"
#include "pmm.h"
#include "proc/proc.h"
#include "vma.h"

vmm_space_t g_kernel_space;

#define VMM_MAX_SPACES 256
static vmm_space_t g_pool[VMM_MAX_SPACES];
static bool g_pool_used[VMM_MAX_SPACES];
static spinlock_t g_pool_lock = SPINLOCK_INIT;
static volatile uint64_t g_kernel_map_generation = 1;

#define PML4_IDX(va) (((va) >> 39) & 0x1FFull)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FFull)
#define PD_IDX(va) (((va) >> 21) & 0x1FFull)
#define PT_IDX(va) (((va) >> 12) & 0x1FFull)

static inline uint64_t pte_addr(uint64_t pte) { return pte & PTE_ADDR_MASK; }

/* intermediate entries always PRESENT|WRITE|USER; leaf PTEs restrict access */
static uint64_t *descend(uint64_t *parent, uint64_t idx) {
    if (!(parent[idx] & VMM_PRESENT)) {
        uint64_t child_phys = (uint64_t) pmm_alloc_zeroed();
        if (!child_phys) return NULL;
        parent[idx] = child_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;
    }
    return (uint64_t *) phys_to_virt(pte_addr(parent[idx]));
}

void vmm_init(void) {
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | (1ULL << 11)); /* enable NX  */

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_space.pml4_phys = cr3 & PTE_ADDR_MASK;

    log_info("VMM: PML4=0x%016lx  NX enabled", g_kernel_space.pml4_phys);
}

int vmm_map(vmm_space_t *sp, uint64_t virt, uint64_t phys, uint64_t flags) {
    // a user-accessible mapping must never land in the kernel half 256+ pml4
    if ((flags & VMM_USER) && virt >= USER_LIMIT) return -1;

    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    bool new_kernel_slot =
        sp == &g_kernel_space && virt >= USER_LIMIT && !(pml4[PML4_IDX(virt)] & VMM_PRESENT);

    uint64_t *pdpt = descend(pml4, PML4_IDX(virt));
    if (!pdpt) return -1;
    uint64_t *pd = descend(pdpt, PDPT_IDX(virt));
    if (!pd) return -1;
    uint64_t *pt = descend(pd, PD_IDX(virt));
    if (!pt) return -1;

    if (pt[PT_IDX(virt)] & VMM_PRESENT) return -1;
    pt[PT_IDX(virt)] = (phys & PTE_ADDR_MASK) | (flags & PTE_FLAGS_MASK) | VMM_PRESENT;
    if (new_kernel_slot)
        __atomic_add_fetch(&g_kernel_map_generation, 1, __ATOMIC_RELEASE);

    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

uint64_t vmm_virt_to_phys(vmm_space_t *sp, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    if (!(pt[PT_IDX(virt)] & VMM_PRESENT)) return 0;
    return pte_addr(pt[PT_IDX(virt)]);
}

/* full leaf PTE (with flags) for a VA, or 0 if any level is not present */
static uint64_t vmm_leaf_pte(vmm_space_t *sp, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    return pt[PT_IDX(virt)];
}

/* verify every page in [virt, virt+len) is mapped and user-accessible */
bool vmm_user_range_ok(vmm_space_t *sp, uint64_t virt, uint64_t len, bool write) {
    if (!sp) return false;
    if (len == 0) return true;
    if (virt + len < virt) return false; /* address overflow */
    uint64_t pg = virt & ~0xFFFULL;
    uint64_t last = (virt + len - 1) & ~0xFFFULL;
    for (;; pg += 0x1000) {
        uint64_t pte = vmm_leaf_pte(sp, pg);
        if (!(pte & VMM_PRESENT) || !(pte & VMM_USER)) return false;
        if (write && !(pte & VMM_WRITE)) return false;
        if (pg == last) break;
    }
    return true;
}

bool vmm_user_range_fault_in(vmm_space_t *sp, uint64_t virt, uint64_t len, bool write) {
    if (!sp) return false;
    if (len == 0) return true;
    if (virt + len < virt) return false;

    /*
     * The accessor reference keeps the address space from being mutated while
     * this walk runs. It must be released again as soon as the walk is over:
     * holding it for the whole syscall makes every mmap()/brk() in a sibling
     * thread fail, which userspace allocators report as out of memory.
     */
    bool guard = sp != &g_kernel_space;
    if (guard) {
        for (;;) {
            if (__atomic_load_n(&sp->user_mutating, __ATOMIC_ACQUIRE)) {
                cpu_relax();
                continue;
            }
            __atomic_add_fetch(&sp->user_accessors, 1, __ATOMIC_ACQ_REL);
            if (!__atomic_load_n(&sp->user_mutating, __ATOMIC_ACQUIRE)) break;
            __atomic_sub_fetch(&sp->user_accessors, 1, __ATOMIC_ACQ_REL);
        }
    }
    while (!__sync_bool_compare_and_swap(&sp->fault_lock, 0, 1)) cpu_relax();
    uint64_t pg = virt & ~0xFFFULL;
    uint64_t last = (virt + len - 1) & ~0xFFFULL;
    for (;; pg += 0x1000) {
        uint64_t pte = vmm_leaf_pte(sp, pg);
        if (pte & VMM_PRESENT) {
            if (!(pte & VMM_USER)) goto fail;
            if (write && !(pte & VMM_WRITE)) {
                if (!(pte & VMM_COW) || vmm_handle_cow_fault(sp, pg) <= 0)
                    goto fail;
            }
        } else {
            /* not present: only ok if a vma covers it - then fault it in now */
            if (!vma_page_fault_allowed(sp, pg, write, false)) goto fail;
            void *phys = pmm_alloc_zeroed();
            if (!phys) goto fail;
            uint64_t flags = vma_page_flags(sp, pg);
            if (vmm_map(sp, pg, (uint64_t) phys, flags) != 0) {
                pmm_free(phys);
                goto fail;
            }
        }
        if (pg == last) break;
    }
    __atomic_store_n(&sp->fault_lock, 0, __ATOMIC_RELEASE);
    if (guard) __atomic_sub_fetch(&sp->user_accessors, 1, __ATOMIC_ACQ_REL);
    return true;
fail:
    __atomic_store_n(&sp->fault_lock, 0, __ATOMIC_RELEASE);
    if (guard) __atomic_sub_fetch(&sp->user_accessors, 1, __ATOMIC_ACQ_REL);
    return false;
}

void vmm_syscall_access_begin(void) {
    proc_t *p = g_current_proc;
    if (!p) return;
    p->user_access_count = 0;
    memset(p->user_access_spaces, 0, sizeof(p->user_access_spaces));
    p->user_access_tracking = 1;
}

void vmm_syscall_access_end(void) {
    proc_t *p = g_current_proc;
    if (!p || !p->user_access_tracking) return;
    uint8_t count = p->user_access_count;
    p->user_access_count = 0;
    p->user_access_tracking = 0;
    for (uint8_t i = 0; i < count; i++) {
        vmm_space_t *sp = p->user_access_spaces[i];
        p->user_access_spaces[i] = NULL;
        if (sp) __atomic_sub_fetch(&sp->user_accessors, 1, __ATOMIC_ACQ_REL);
    }
}

/* Waits out concurrent user-memory walks instead of failing: callers turn a
   refusal into EAGAIN, and userspace treats that as an allocation failure. */
bool vmm_space_mutation_begin(vmm_space_t *sp) {
    if (!sp || sp == &g_kernel_space) return true;
    for (int attempt = 0; attempt < 1000000; attempt++) {
        if (__sync_bool_compare_and_swap(&sp->user_mutating, 0, 1)) {
            if (!__atomic_load_n(&sp->user_accessors, __ATOMIC_ACQUIRE)) return true;
            __atomic_store_n(&sp->user_mutating, 0, __ATOMIC_RELEASE);
        }
        cpu_relax();
    }
    return false;
}

void vmm_space_mutation_end(vmm_space_t *sp) {
    if (sp && sp != &g_kernel_space)
        __atomic_store_n(&sp->user_mutating, 0, __ATOMIC_RELEASE);
}

int vmm_protect(vmm_space_t *sp, uint64_t virt, uint64_t flags) {
    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    if (!(pt[PT_IDX(virt)] & VMM_PRESENT)) return -1;
    pt[PT_IDX(virt)] = pte_addr(pt[PT_IDX(virt)]) | (flags & PTE_FLAGS_MASK) | VMM_PRESENT;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

void vmm_unmap(vmm_space_t *sp, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);

    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));

    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return;
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));

    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return;
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));

    pt[PT_IDX(virt)] = 0;

    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

vmm_space_t *vmm_space_new(void) {
    int slot = -1;
    spin_lock(&g_pool_lock);
    for (int i = 0; i < VMM_MAX_SPACES; i++) {
        if (!g_pool_used[i]) {
            slot = i;
            g_pool_used[i] = true;
            break;
        }
    }
    spin_unlock(&g_pool_lock);
    if (slot < 0) return NULL;

    uint64_t pml4_phys = (uint64_t) pmm_alloc_zeroed();
    if (!pml4_phys) {
        spin_lock(&g_pool_lock);
        g_pool_used[slot] = false;
        spin_unlock(&g_pool_lock);
        return NULL;
    }

    // share kernel half (pml4 256-511); user half starts zeroed
    uint64_t *new_pml4 = (uint64_t *) phys_to_virt(pml4_phys);
    uint64_t *kern_pml4 = (uint64_t *) phys_to_virt(g_kernel_space.pml4_phys);
    for (int i = 256; i < 512; i++) new_pml4[i] = kern_pml4[i];

    memset(&g_pool[slot], 0, sizeof(g_pool[slot]));
    g_pool[slot].pml4_phys = pml4_phys;
    g_pool[slot].kernel_map_generation =
        __atomic_load_n(&g_kernel_map_generation, __ATOMIC_ACQUIRE);
    vma_reset(&g_pool[slot]);
    __atomic_store_n(&g_pool[slot].refcount, 1, __ATOMIC_RELEASE);
    return &g_pool[slot];
}

void vmm_space_retain(vmm_space_t *sp) {
    if (!sp || sp == &g_kernel_space) return;
    __atomic_add_fetch(&sp->refcount, 1, __ATOMIC_ACQ_REL);
}

static void free_pt(uint64_t *pt) {
    for (int i = 0; i < 512; i++)
        if (pt[i] & VMM_PRESENT) pmm_free((void *) pte_addr(pt[i]));
    pmm_free((void *) virt_to_phys(pt));
}

static void free_pd(uint64_t *pd) {
    for (int i = 0; i < 512; i++)
        if (pd[i] & VMM_PRESENT) free_pt((uint64_t *) phys_to_virt(pte_addr(pd[i])));
    pmm_free((void *) virt_to_phys(pd));
}

static void free_pdpt(uint64_t *pdpt) {
    for (int i = 0; i < 512; i++)
        if (pdpt[i] & VMM_PRESENT) free_pd((uint64_t *) phys_to_virt(pte_addr(pdpt[i])));
    pmm_free((void *) virt_to_phys(pdpt));
}

void vmm_space_free(vmm_space_t *sp) {
    if (!sp || sp == &g_kernel_space) return;
    if (__atomic_fetch_sub(&sp->refcount, 1, __ATOMIC_ACQ_REL) != 1) return;

    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);

    for (int i = 0; i < 256; i++) /* user half only; kernel half is shared */
        if (pml4[i] & VMM_PRESENT) free_pdpt((uint64_t *) phys_to_virt(pte_addr(pml4[i])));

    pmm_free((void *) sp->pml4_phys);

    spin_lock(&g_pool_lock);
    for (int i = 0; i < VMM_MAX_SPACES; i++) {
        if (&g_pool[i] == sp) {
            g_pool_used[i] = false;
            break;
        }
    }
    spin_unlock(&g_pool_lock);
}

void vmm_switch(vmm_space_t *sp) {
    if (sp != &g_kernel_space) {
        uint64_t generation = __atomic_load_n(&g_kernel_map_generation, __ATOMIC_ACQUIRE);
        if (sp->kernel_map_generation != generation) {
            uint64_t *dst = (uint64_t *) phys_to_virt(sp->pml4_phys);
            uint64_t *src = (uint64_t *) phys_to_virt(g_kernel_space.pml4_phys);
            for (int i = 256; i < 512; i++) dst[i] = src[i];
            sp->kernel_map_generation = generation;
        }
    }
    uint64_t current;
    __asm__ volatile("mov %%cr3, %0" : "=r"(current));
    if ((current & PTE_ADDR_MASK) != sp->pml4_phys)
        __asm__ volatile("mov %0, %%cr3" ::"r"(sp->pml4_phys) : "memory");
}

int vmm_fork_user(vmm_space_t *dst, vmm_space_t *src) {
    vma_copy(dst, src); /* vma metadata; pages copied by the page-table walk below */
    uint64_t *src_pml4 = (uint64_t *) phys_to_virt(src->pml4_phys);

    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & VMM_PRESENT)) continue;
        uint64_t *src_pdpt = (uint64_t *) phys_to_virt(pte_addr(src_pml4[i]));

        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & VMM_PRESENT)) continue;
            uint64_t *src_pd = (uint64_t *) phys_to_virt(pte_addr(src_pdpt[j]));

            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & VMM_PRESENT)) continue;
                uint64_t *src_pt = (uint64_t *) phys_to_virt(pte_addr(src_pd[k]));

                for (int l = 0; l < 512; l++) {
                    uint64_t pte = src_pt[l];
                    if (!(pte & VMM_PRESENT)) continue;

                    uint64_t va = ((uint64_t) i << 39) | ((uint64_t) j << 30) |
                                  ((uint64_t) k << 21) | ((uint64_t) l << 12);

                    void *new_phys = pmm_alloc();
                    if (!new_phys) return -1;
                    memcpy(phys_to_virt((uint64_t) new_phys), phys_to_virt(pte_addr(pte)),
                           PAGE_SIZE);

                    if (vmm_map(dst, va, (uint64_t) new_phys, pte & PTE_FLAGS_MASK) < 0) {
                        pmm_free(new_phys);
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

int vmm_fork_user_cow(vmm_space_t *dst, vmm_space_t *src) {
    if (!dst || !src) return -1;
    vma_copy(dst, src);
    uint64_t *src_pml4 = (uint64_t *) phys_to_virt(src->pml4_phys);

    for (int i = 0; i < 256; i++) {
        if (!(src_pml4[i] & VMM_PRESENT)) continue;
        uint64_t *src_pdpt = (uint64_t *) phys_to_virt(pte_addr(src_pml4[i]));
        for (int j = 0; j < 512; j++) {
            if (!(src_pdpt[j] & VMM_PRESENT)) continue;
            uint64_t *src_pd = (uint64_t *) phys_to_virt(pte_addr(src_pdpt[j]));
            for (int k = 0; k < 512; k++) {
                if (!(src_pd[k] & VMM_PRESENT)) continue;
                uint64_t *src_pt = (uint64_t *) phys_to_virt(pte_addr(src_pd[k]));
                for (int l = 0; l < 512; l++) {
                    uint64_t pte = src_pt[l];
                    if (!(pte & VMM_PRESENT)) continue;

                    uint64_t va = ((uint64_t) i << 39) | ((uint64_t) j << 30) |
                                  ((uint64_t) k << 21) | ((uint64_t) l << 12);
                    uint64_t flags = pte & PTE_FLAGS_MASK;
                    uint32_t map_flags = 0;
                    bool owned = false;
                    bool tracked =
                        vma_range_info(src, va, PAGE_SIZE, NULL, &map_flags, &owned);

                    /*
                     * Device mappings and legacy untracked mappings may not use
                     * PMM-managed frames. Preserve the old eager-copy behavior.
                     */
                    if (!tracked || !owned ||
                        !pmm_retain((void *) pte_addr(pte))) {
                        void *new_phys = pmm_alloc();
                        if (!new_phys) return -1;
                        memcpy(phys_to_virt((uint64_t) new_phys),
                               phys_to_virt(pte_addr(pte)), PAGE_SIZE);
                        if (vmm_map(dst, va, (uint64_t) new_phys, flags) < 0) {
                            pmm_free(new_phys);
                            return -1;
                        }
                        continue;
                    }

                    bool private_write =
                        (flags & VMM_WRITE) && !(map_flags & VMA_MAP_SHARED);
                    uint64_t child_flags = flags;
                    if (private_write) {
                        child_flags &= ~(uint64_t) VMM_WRITE;
                        child_flags |= VMM_COW;
                    }
                    if (vmm_map(dst, va, pte_addr(pte), child_flags) < 0) {
                        pmm_free((void *) pte_addr(pte));
                        return -1;
                    }
                    if (private_write) {
                        src_pt[l] = pte_addr(pte) | child_flags | VMM_PRESENT;
                        __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
                    }
                }
            }
        }
    }
    return 0;
}

int vmm_handle_cow_fault(vmm_space_t *sp, uint64_t virt) {
    uint64_t pte = vmm_leaf_pte(sp, virt & PAGE_MASK);
    if (!(pte & VMM_PRESENT) || !(pte & VMM_COW)) return 0;

    uint64_t flags = (pte & PTE_FLAGS_MASK) | VMM_WRITE;
    flags &= ~(uint64_t) VMM_COW;
    if (pmm_ref_count((void *) pte_addr(pte)) == 1)
        return vmm_protect(sp, virt & PAGE_MASK, flags) == 0 ? 1 : -1;

    void *new_phys = pmm_alloc();
    if (!new_phys) return -1;
    memcpy(phys_to_virt((uint64_t) new_phys), phys_to_virt(pte_addr(pte)), PAGE_SIZE);

    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    pt[PT_IDX(virt)] = (uint64_t) new_phys | (flags & PTE_FLAGS_MASK) | VMM_PRESENT;
    pmm_free((void *) pte_addr(pte));
    __asm__ volatile("invlpg (%0)" :: "r"(virt & PAGE_MASK) : "memory");
    return 1;
}

int vmm_phantom_relax_page(vmm_space_t *sp, uint64_t virt, bool write, bool execute) {
    if (!sp || (!write && !execute)) return -1;
    uint64_t page = virt & PAGE_MASK;
    uint64_t pte = vmm_leaf_pte(sp, page);
    if (!(pte & VMM_PRESENT) || !(pte & VMM_USER)) return -1;

    void *new_phys = pmm_alloc();
    if (!new_phys) return -1;
    memcpy(phys_to_virt((uint64_t) new_phys), phys_to_virt(pte_addr(pte)), PAGE_SIZE);

    uint64_t flags = pte & PTE_FLAGS_MASK;
    if (write || (flags & VMM_COW)) flags |= VMM_WRITE;
    if (execute) flags &= ~(uint64_t) VMM_NX;
    flags &= ~(uint64_t) VMM_COW;

    uint64_t *pml4 = (uint64_t *) phys_to_virt(sp->pml4_phys);
    uint64_t *pdpt = (uint64_t *) phys_to_virt(pte_addr(pml4[PML4_IDX(page)]));
    uint64_t *pd = (uint64_t *) phys_to_virt(pte_addr(pdpt[PDPT_IDX(page)]));
    uint64_t *pt = (uint64_t *) phys_to_virt(pte_addr(pd[PD_IDX(page)]));
    pt[PT_IDX(page)] = (uint64_t) new_phys | flags | VMM_PRESENT;
    pmm_free((void *) pte_addr(pte));
    return 0;
}
