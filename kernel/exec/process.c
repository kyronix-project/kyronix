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

uint64_t kern_rand64(void)
{
    static uint64_t s;
    if (!s)
        s = g_ticks ^ 0xdeadbeef13579aceULL;
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
}

#define STACK_MAX_ARGS 4096

/* write bytes into user stack pages, handling page boundaries */
static void stack_write(uint64_t uva, const void* src, uint64_t len, uint64_t* phys_arr,
                        uint64_t stack_base)
{
    const uint8_t* s = (const uint8_t*) src;
    while (len) {
        uint64_t off = uva - stack_base;
        int pg = (int) (off >> 12);
        uint64_t in_pg = off & 0xFFFULL;
        uint64_t chunk = PAGE_SIZE - in_pg;
        if (chunk > len)
            chunk = len;
        memcpy((uint8_t*) phys_to_virt(phys_arr[pg]) + in_pg, s, chunk);
        uva += chunk;
        s += chunk;
        len -= chunk;
    }
}

static void stack_write_u64(uint64_t uva, uint64_t val, uint64_t* phys_arr, uint64_t stack_base)
{
    stack_write(uva, &val, 8, phys_arr, stack_base);
}

uint64_t setup_user_stack(vmm_space_t* space, const elf_load_result_t* elf, int argc,
                          const char* const* argv, const char* const* envp)
{
    int envc = 0;
    if (envp)
        while (envp[envc])
            envc++;

    if (argc > STACK_MAX_ARGS || envc > STACK_MAX_ARGS)
        return 0;

    uint64_t stack_top = USER_STACK_TOP - ((kern_rand64() & 0xFFULL) << 12);
    uint64_t stack_base = stack_top - (uint64_t) USER_STACK_PAGES * PAGE_SIZE;

    uint64_t phys[USER_STACK_PAGES];
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        phys[i] = (uint64_t) pmm_alloc_zeroed();
        if (!phys[i]) {
            for (int j = 0; j < i; j++)
                pmm_free((void*) phys[j]);
            return 0;
        }
        uint64_t va = stack_base + (uint64_t) i * PAGE_SIZE;
        if (vmm_map(space, va, phys[i], VMM_UDATA) < 0) {
            for (int j = 0; j <= i; j++)
                pmm_free((void*) phys[j]);
            return 0;
        }
    }

    uint64_t* env_uva = envc ? (uint64_t*) kmalloc((uint64_t) envc * 8) : NULL;
    uint64_t* arg_uva = (uint64_t*) kmalloc((uint64_t) (argc + 1) * 8);
    if (!arg_uva || (envc && !env_uva)) {
        kfree(env_uva);
        kfree(arg_uva);
        return 0;
    }
    memset(arg_uva, 0, (uint64_t) (argc + 1) * 8);
    if (envc)
        memset(env_uva, 0, (uint64_t) envc * 8);

    uint64_t sp = stack_top;
    sp -= 16;
    uint64_t random_uva = sp;

    for (int i = envc - 1; i >= 0; i--) {
        uint64_t len = (uint64_t) strlen(envp[i]) + 1;
        sp -= len;
        sp &= ~(uint64_t) 7;
        if (sp < stack_base) {
            kfree(env_uva);
            kfree(arg_uva);
            return 0;
        }
        env_uva[i] = sp;
        stack_write(sp, envp[i], len, phys, stack_base);
    }

    for (int i = argc - 1; i >= 0; i--) {
        if (!argv || !argv[i])
            continue;
        uint64_t len = (uint64_t) strlen(argv[i]) + 1;
        sp -= len;
        sp &= ~(uint64_t) 7;
        if (sp < stack_base) {
            kfree(env_uva);
            kfree(arg_uva);
            return 0;
        }
        arg_uva[i] = sp;
        stack_write(sp, argv[i], len, phys, stack_base);
    }

    uint64_t auxv[] = {
        AT_ENTRY,  elf->prog_entry,
        AT_PHDR,   elf->phdr_va,
        AT_PHENT,  elf->phentsize,
        AT_PHNUM,  elf->phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_RANDOM, random_uva,
        AT_BASE,   elf->interp_base,
        AT_EXECFN, argc > 0 ? arg_uva[0] : 0,
        AT_NULL,   0,
    };

    uint64_t frame_bytes = (uint64_t) (1 + argc + 1 + envc + 1) * 8 + sizeof(auxv);
    sp -= frame_bytes;
    sp &= ~(uint64_t) 15;
    if (sp < stack_base) {
        kfree(env_uva);
        kfree(arg_uva);
        return 0;
    }

    uint64_t wp = sp;
    stack_write_u64(wp, (uint64_t) argc, phys, stack_base);
    wp += 8;
    for (int i = 0; i < argc; i++) {
        stack_write_u64(wp, arg_uva[i], phys, stack_base);
        wp += 8;
    }
    stack_write_u64(wp, 0, phys, stack_base);
    wp += 8;
    for (int i = 0; i < envc; i++) {
        stack_write_u64(wp, env_uva[i], phys, stack_base);
        wp += 8;
    }
    stack_write_u64(wp, 0, phys, stack_base);
    wp += 8;
    stack_write(wp, auxv, sizeof(auxv), phys, stack_base);

    log_info("Stack: RSP=0x%lx  argc=%d  argv0=%s", sp, argc,
             (argc > 0 && argv && argv[0]) ? argv[0] : "(null)");
    kfree(env_uva);
    kfree(arg_uva);
    return sp;
}

int process_exec(const void* data, uint64_t size, const char* name)
{
    elf_load_result_t res;
    if (elf_load(data, size, &res) < 0) {
        log_error("process_exec: elf_load failed");
        return -1;
    }

    const char* init_argv[] = {name, NULL};
    const char* init_envp[] = {"TERM=vt100", "HOME=/", "PATH=/:/bin:/usr/bin", "SHELL=/init", NULL};
    uint64_t rsp = setup_user_stack(res.space, &res, 1, init_argv, init_envp);
    if (!rsp) {
        log_error("process_exec: stack setup failed");
        vmm_space_free(res.space);
        return -1;
    }

    proc_t* p = proc_alloc(0);
    if (!p) {
        log_error("process_exec: proc_alloc failed");
        vmm_space_free(res.space);
        return -1;
    }

    p->space = res.space;
    p->brk = PAGE_ALIGN_UP(res.brk);
    p->brk_base = p->brk;
    p->mmap_bump = 0x0000500000000000ULL + ((kern_rand64() & 0x1FFULL) << 21); /* +-1 GB aslr */
    p->state = PROC_RUNNING;

    vfs_copy_fdtable(p->fds, vfs_get_fdtable());
    vfs_set_fdtable(p->fds);

    g_current_proc = p;
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);

    vmm_switch(p->space);
    enter_userspace(res.entry, rsp, 0x202ULL);
}
