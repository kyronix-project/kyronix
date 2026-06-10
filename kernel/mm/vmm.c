#include "vmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/log.h"
#include "lib/string.h"
#include "pmm.h"

vmm_space_t g_kernel_space;

#define VMM_MAX_SPACES 256
static vmm_space_t g_pool[VMM_MAX_SPACES];
static bool g_pool_used[VMM_MAX_SPACES];

#define PML4_IDX(va) (((va) >> 39) & 0x1FFull)
#define PDPT_IDX(va) (((va) >> 30) & 0x1FFull)
#define PD_IDX(va) (((va) >> 21) & 0x1FFull)
#define PT_IDX(va) (((va) >> 12) & 0x1FFull)

static inline uint64_t pte_addr(uint64_t pte)
{
    return pte & PTE_ADDR_MASK;
}

/* intermediate entries always PRESENT|WRITE|USER; leaf PTEs restrict access */
static uint64_t* descend(uint64_t* parent, uint64_t idx)
{
    if (!(parent[idx] & VMM_PRESENT))
    {
        uint64_t child_phys = (uint64_t) pmm_alloc_zeroed();
        if (!child_phys)
            return NULL;
        parent[idx] = child_phys | VMM_PRESENT | VMM_WRITE | VMM_USER;
    }
    return (uint64_t*) phys_to_virt(pte_addr(parent[idx]));
}

void vmm_init(void)
{
    uint64_t efer = rdmsr(0xC0000080);
    wrmsr(0xC0000080, efer | (1ULL << 11)); /* enable NX (EFER.NXE) */

    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    g_kernel_space.pml4_phys = cr3 & PTE_ADDR_MASK;

    log_info("VMM: PML4=0x%016lx  NX enabled", g_kernel_space.pml4_phys);
}

int vmm_map(vmm_space_t* sp, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t* pml4 = (uint64_t*) phys_to_virt(sp->pml4_phys);

    uint64_t* pdpt = descend(pml4, PML4_IDX(virt));
    if (!pdpt)
        return -1;
    uint64_t* pd = descend(pdpt, PDPT_IDX(virt));
    if (!pd)
        return -1;
    uint64_t* pt = descend(pd, PD_IDX(virt));
    if (!pt)
        return -1;

    pt[PT_IDX(virt)] = (phys & PTE_ADDR_MASK) | (flags & PTE_FLAGS_MASK) | VMM_PRESENT;

    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

uint64_t vmm_virt_to_phys(vmm_space_t* sp, uint64_t virt)
{
    uint64_t* pml4 = (uint64_t*) phys_to_virt(sp->pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT))
        return 0;
    uint64_t* pdpt = (uint64_t*) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT))
        return 0;
    uint64_t* pd = (uint64_t*) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT))
        return 0;
    uint64_t* pt = (uint64_t*) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    if (!(pt[PT_IDX(virt)] & VMM_PRESENT))
        return 0;
    return pte_addr(pt[PT_IDX(virt)]);
}

int vmm_protect(vmm_space_t* sp, uint64_t virt, uint64_t flags)
{
    uint64_t* pml4 = (uint64_t*) phys_to_virt(sp->pml4_phys);
    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t* pdpt = (uint64_t*) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));
    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t* pd = (uint64_t*) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));
    if (!(pd[PD_IDX(virt)] & VMM_PRESENT)) return -1;
    uint64_t* pt = (uint64_t*) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));
    if (!(pt[PT_IDX(virt)] & VMM_PRESENT)) return -1;
    pt[PT_IDX(virt)] = pte_addr(pt[PT_IDX(virt)]) | (flags & PTE_FLAGS_MASK) | VMM_PRESENT;
    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return 0;
}

void vmm_unmap(vmm_space_t* sp, uint64_t virt)
{
    uint64_t* pml4 = (uint64_t*) phys_to_virt(sp->pml4_phys);

    if (!(pml4[PML4_IDX(virt)] & VMM_PRESENT))
        return;
    uint64_t* pdpt = (uint64_t*) phys_to_virt(pte_addr(pml4[PML4_IDX(virt)]));

    if (!(pdpt[PDPT_IDX(virt)] & VMM_PRESENT))
        return;
    uint64_t* pd = (uint64_t*) phys_to_virt(pte_addr(pdpt[PDPT_IDX(virt)]));

    if (!(pd[PD_IDX(virt)] & VMM_PRESENT))
        return;
    uint64_t* pt = (uint64_t*) phys_to_virt(pte_addr(pd[PD_IDX(virt)]));

    pt[PT_IDX(virt)] = 0;

    __asm__ volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

vmm_space_t* vmm_space_new(void)
{
    int slot = -1;
    for (int i = 0; i < VMM_MAX_SPACES; i++)
    {
        if (!g_pool_used[i])
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return NULL;

    uint64_t pml4_phys = (uint64_t) pmm_alloc_zeroed();
    if (!pml4_phys)
        return NULL;

    /* share kernel half (PML4 entries 256-511); user half starts zeroed */
    uint64_t* new_pml4 = (uint64_t*) phys_to_virt(pml4_phys);
    uint64_t* kern_pml4 = (uint64_t*) phys_to_virt(g_kernel_space.pml4_phys);
    for (int i = 256; i < 512; i++)
        new_pml4[i] = kern_pml4[i];

    g_pool[slot].pml4_phys = pml4_phys;
    g_pool_used[slot] = true;
    return &g_pool[slot];
}

static void free_pt(uint64_t* pt)
{
    for (int i = 0; i < 512; i++)
        if (pt[i] & VMM_PRESENT)
            pmm_free((void*) pte_addr(pt[i]));
    pmm_free((void*) virt_to_phys(pt));
}

static void free_pd(uint64_t* pd)
{
    for (int i = 0; i < 512; i++)
        if (pd[i] & VMM_PRESENT)
            free_pt((uint64_t*) phys_to_virt(pte_addr(pd[i])));
    pmm_free((void*) virt_to_phys(pd));
}

static void free_pdpt(uint64_t* pdpt)
{
    for (int i = 0; i < 512; i++)
        if (pdpt[i] & VMM_PRESENT)
            free_pd((uint64_t*) phys_to_virt(pte_addr(pdpt[i])));
    pmm_free((void*) virt_to_phys(pdpt));
}

void vmm_space_free(vmm_space_t* sp)
{
    if (sp == &g_kernel_space)
        return;

    uint64_t* pml4 = (uint64_t*) phys_to_virt(sp->pml4_phys);

    for (int i = 0; i < 256; i++) /* user half only; kernel half is shared */
        if (pml4[i] & VMM_PRESENT)
            free_pdpt((uint64_t*) phys_to_virt(pte_addr(pml4[i])));

    pmm_free((void*) sp->pml4_phys);

    for (int i = 0; i < VMM_MAX_SPACES; i++)
    {
        if (&g_pool[i] == sp)
        {
            g_pool_used[i] = false;
            break;
        }
    }
}

void vmm_switch(vmm_space_t* sp)
{
    if (sp != &g_kernel_space)
    {
        /* sync any kernel mappings added after vmm_space_new (e.g. kstacks) */
        uint64_t* dst = (uint64_t*) phys_to_virt(sp->pml4_phys);
        uint64_t* src = (uint64_t*) phys_to_virt(g_kernel_space.pml4_phys);
        for (int i = 256; i < 512; i++)
            dst[i] = src[i];
    }
    __asm__ volatile("mov %0, %%cr3" ::"r"(sp->pml4_phys) : "memory");
}

int vmm_fork_user(vmm_space_t* dst, vmm_space_t* src)
{
    uint64_t* src_pml4 = (uint64_t*) phys_to_virt(src->pml4_phys);

    for (int i = 0; i < 256; i++)
    {
        if (!(src_pml4[i] & VMM_PRESENT))
            continue;
        uint64_t* src_pdpt = (uint64_t*) phys_to_virt(pte_addr(src_pml4[i]));

        for (int j = 0; j < 512; j++)
        {
            if (!(src_pdpt[j] & VMM_PRESENT))
                continue;
            uint64_t* src_pd = (uint64_t*) phys_to_virt(pte_addr(src_pdpt[j]));

            for (int k = 0; k < 512; k++)
            {
                if (!(src_pd[k] & VMM_PRESENT))
                    continue;
                uint64_t* src_pt = (uint64_t*) phys_to_virt(pte_addr(src_pd[k]));

                for (int l = 0; l < 512; l++)
                {
                    uint64_t pte = src_pt[l];
                    if (!(pte & VMM_PRESENT))
                        continue;

                    uint64_t va = ((uint64_t) i << 39) | ((uint64_t) j << 30) |
                                  ((uint64_t) k << 21) | ((uint64_t) l << 12);

                    void* new_phys = pmm_alloc();
                    if (!new_phys)
                        return -1;
                    memcpy(phys_to_virt((uint64_t) new_phys), phys_to_virt(pte_addr(pte)),
                           PAGE_SIZE);

                    uint64_t flags = pte & PTE_FLAGS_MASK;
                    if (vmm_map(dst, va, (uint64_t) new_phys, flags) < 0)
                    {
                        pmm_free(new_phys);
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}
