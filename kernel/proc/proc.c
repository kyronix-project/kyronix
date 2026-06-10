#include "proc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/syscall_setup.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "syscall/syscall.h"

/* kernel stack VAs: one guard page + KSTACK_PAGES per process */
#define KSTACK_VA_BASE 0xffff920000000000ULL
#define KSTACK_VA_STRIDE ((KSTACK_PAGES + 1) * PAGE_SIZE)

static uint64_t g_kstack_va_bump = KSTACK_VA_BASE;

proc_t g_proctable[PROC_MAX];
proc_t* g_current_proc = NULL;

void proc_init(void)
{
    memset(g_proctable, 0, sizeof(g_proctable));
    log_info("PROC: table initialised  (%d slots)", PROC_MAX);
}

proc_t* proc_alloc(uint32_t ppid)
{
    for (int i = 0; i < PROC_MAX; i++)
    {
        if (g_proctable[i].state != PROC_UNUSED)
            continue;

        proc_t* p = &g_proctable[i];
        memset(p, 0, sizeof(*p));

        p->state = PROC_READY;
        p->pid = (uint32_t) (i + 1);
        p->ppid = ppid;
        p->pgid = (uint32_t) (i + 1);
        p->wait_for = 0;

        uint64_t guard_va = g_kstack_va_bump;
        g_kstack_va_bump += KSTACK_VA_STRIDE;
        for (int pg = 0; pg < KSTACK_PAGES; pg++)
        {
            void* phys = pmm_alloc_zeroed();
            if (!phys)
            {
                /* free already-mapped pages */
                for (int j = 0; j < pg; j++)
                {
                    uint64_t va = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
                    vmm_unmap(&g_kernel_space, va);
                    if (pa) pmm_free((void*) pa);
                }
                p->state = PROC_UNUSED;
                return NULL;
            }
            uint64_t va = guard_va + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
            if (vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0)
            {
                pmm_free(phys);
                for (int j = 0; j < pg; j++)
                {
                    uint64_t jva = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, jva);
                    vmm_unmap(&g_kernel_space, jva);
                    if (pa) pmm_free((void*) pa);
                }
                p->state = PROC_UNUSED;
                return NULL;
            }
        }
        p->kstack_guard = guard_va;
        p->kstack       = (uint8_t*)(guard_va + PAGE_SIZE);
        p->kstack_top   = guard_va + KSTACK_VA_STRIDE;

        p->fds = (vfs_file_t**) kcalloc(VFS_FD_MAX, sizeof(vfs_file_t*));
        if (!p->fds)
        {
            kfree(p->kstack);
            p->state = PROC_UNUSED;
            return NULL;
        }

        p->mmap_bump = 0x0000500000000000ULL;
        p->cwd[0] = '/';
        p->cwd[1] = '\0';
        return p;
    }
    return NULL;
}

void proc_kstack_free(proc_t* p)
{
    if (!p->kstack_guard)
        return;
    for (int pg = 0; pg < KSTACK_PAGES; pg++)
    {
        uint64_t va = p->kstack_guard + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
        uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
        vmm_unmap(&g_kernel_space, va);
        if (pa) pmm_free((void*) pa);
    }
    p->kstack_guard = 0;
    p->kstack = NULL;
}

proc_t* proc_find(uint32_t pid)
{
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && g_proctable[i].pid == pid)
            return &g_proctable[i];
    return NULL;
}

proc_t* proc_next_ready(proc_t* skip)
{
    for (int i = 0; i < PROC_MAX; i++)
    {
        if (&g_proctable[i] == skip)
            continue;
        if (g_proctable[i].state == PROC_READY)
            return &g_proctable[i];
    }
    return NULL;
}

void sched_yield_blocking(void)
{
    proc_t* p = g_current_proc;
    proc_t* next = proc_next_ready(p);
    if (!next) {
        sti(); hlt(); cli(); /* let IRQ0/IRQ1 fire when idle */
        return;
    }

    p->state = PROC_WAITING;
    next->state = PROC_RUNNING;
    vfs_set_fdtable(next->fds);
    g_current_space = next->space;
    cpu_set_kernel_stack(next->kstack_top);
    sched_switch(next);

    p->state = PROC_RUNNING;
    vfs_set_fdtable(p->fds);
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);
}
