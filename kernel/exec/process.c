#include "process.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "elf.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x7fffffff0000ULL
#define USER_STACK_BASE (USER_STACK_TOP - (uint64_t) USER_STACK_PAGES * PAGE_SIZE)

uint64_t kern_rand64(void)
{
    static uint64_t s;
    if (!s) s = g_ticks ^ 0xdeadbeef13579aceULL;
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;
    return s;
}

static inline void* top_page_kva(uint64_t uva, uint64_t top_page_phys, uint64_t stack_top)
{
    return (uint8_t*) phys_to_virt(top_page_phys) + (uva - (stack_top - PAGE_SIZE));
}

uint64_t setup_user_stack(vmm_space_t* space, const elf_load_result_t* elf, int argc,
                          const char* const* argv, const char* const* envp)
{
    int envc = 0;
    if (envp)
        while (envp[envc])
            envc++;

    /* ASLR: shift stack down by 0-255 pages */
    uint64_t stack_top = USER_STACK_TOP - ((kern_rand64() & 0xFFULL) << 12);

    uint64_t phys[USER_STACK_PAGES];
    for (int i = 0; i < USER_STACK_PAGES; i++)
    {
        phys[i] = (uint64_t) pmm_alloc_zeroed();
        if (!phys[i])
        {
            for (int j = 0; j < i; j++)
                pmm_free((void*) phys[j]);
            return 0;
        }
        uint64_t va = stack_top - (uint64_t) (USER_STACK_PAGES - i) * PAGE_SIZE;
        if (vmm_map(space, va, phys[i], VMM_UDATA) < 0)
        {
            for (int j = 0; j <= i; j++)
                pmm_free((void*) phys[j]);
            return 0;
        }
    }

    uint64_t top_phys = phys[USER_STACK_PAGES - 1];
    uint64_t sp = stack_top;

    sp -= 16;
    uint64_t random_uva = sp;

    uint64_t env_uva[64] = {0};
    for (int i = envc - 1; i >= 0; i--)
    {
        uint64_t len = (uint64_t) strlen(envp[i]) + 1;
        sp -= len;
        sp &= ~(uint64_t) 7;
        env_uva[i] = sp;
        memcpy(top_page_kva(sp, top_phys, stack_top), envp[i], len);
    }

    uint64_t arg_uva[64] = {0};
    for (int i = argc - 1; i >= 0; i--)
    {
        if (!argv || !argv[i])
            continue;
        uint64_t len = (uint64_t) strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~(uint64_t) 7;
        arg_uva[i] = sp;
        memcpy(top_page_kva(sp, top_phys, stack_top), argv[i], len);
    }

    uint64_t auxv[] = {
        AT_ENTRY,  elf->entry,
        AT_PHDR,   elf->phdr_va,
        AT_PHENT,  elf->phentsize,
        AT_PHNUM,  elf->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_RANDOM, random_uva,
        AT_EXECFN, argc > 0 ? arg_uva[0] : 0,
        AT_NULL,   0,
    };

    uint64_t frame_bytes = (uint64_t) (1 + argc + 1 + envc + 1) * 8 + sizeof(auxv);
    sp -= frame_bytes;
    sp &= ~(uint64_t) 15; /* 16-byte align */

    uint64_t* p = top_page_kva(sp, top_phys, stack_top);
    *p++ = (uint64_t) argc;
    for (int i = 0; i < argc; i++)
        *p++ = arg_uva[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = env_uva[i];
    *p++ = 0;
    memcpy(p, auxv, sizeof(auxv));

    log_info("Stack: RSP=0x%lx  argc=%d  argv0=%s", sp, argc,
             (argc > 0 && argv && argv[0]) ? argv[0] : "(null)");
    return sp;
}

int process_exec(const void* data, uint64_t size, const char* name)
{
    elf_load_result_t res;
    if (elf_load(data, size, &res) < 0)
    {
        log_error("process_exec: elf_load failed");
        return -1;
    }

    const char* init_argv[] = {name, NULL};
    const char* init_envp[] = {"TERM=vt100", "HOME=/", "PATH=/:/bin:/usr/bin", "SHELL=/init", NULL};
    uint64_t rsp = setup_user_stack(res.space, &res, 1, init_argv, init_envp);
    if (!rsp)
    {
        log_error("process_exec: stack setup failed");
        vmm_space_free(res.space);
        return -1;
    }

    proc_t* p = proc_alloc(0);
    if (!p)
    {
        log_error("process_exec: proc_alloc failed");
        vmm_space_free(res.space);
        return -1;
    }

    p->space = res.space;
    p->brk = PAGE_ALIGN_UP(res.brk);
    p->brk_base = p->brk;
    p->mmap_bump = 0x0000500000000000ULL + ((kern_rand64() & 0x1FFULL) << 21); /* ±1 GB ASLR */
    p->state = PROC_RUNNING;

    vfs_copy_fdtable(p->fds, vfs_get_fdtable());
    vfs_set_fdtable(p->fds);

    g_current_proc = p;
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);

    vmm_switch(p->space);
    enter_userspace(res.entry, rsp, 0x202ULL);
}
