#include "syscall.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "exec/elf.h"
#include "exec/process.h"
#include "fs/vfs.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "proc/signal.h"

/* linuh errno values */
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EBADF 9
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define ENOEXEC 8
#define ENOTDIR 20
#define EINVAL 22
#define EMFILE 24
#define ENOSYS 38

vmm_space_t* g_current_space = NULL;

static inline proc_t* cur(void)
{
    return g_current_proc;
}

void syscall_set_brk(uint64_t brk_base)
{
    proc_t* p = cur();
    if (p)
        p->brk = p->brk_base = PAGE_ALIGN_UP(brk_base);
}

static int64_t sys_brk(uint64_t addr)
{
    proc_t* p = cur();
    if (!p || !p->space)
        return -(int64_t) ENOMEM;
    if (addr == 0 || addr <= p->brk)
        return (int64_t) p->brk;

    uint64_t old = PAGE_ALIGN_UP(p->brk);
    uint64_t new = PAGE_ALIGN_UP(addr);
    for (uint64_t va = old; va < new; va += PAGE_SIZE)
    {
        void* ph = pmm_alloc_zeroed();
        if (!ph)
            return (int64_t) p->brk;
        if (vmm_map(p->space, va, (uint64_t) ph, VMM_UDATA) < 0)
        {
            pmm_free(ph);
            return (int64_t) p->brk;
        }
    }
    p->brk = addr;
    return (int64_t) addr;
}

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define MAP_ANON 0x20
#define MAP_FIXED 0x10

static int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                        uint64_t off)
{
    (void) fd;
    (void) off;
    proc_t* p = cur();
    if (!p || !p->space)
        return -(int64_t) ENOMEM;
    if (!(flags & MAP_ANON))
        return -(int64_t) ENOSYS;
    if (!length)
        return -(int64_t) EINVAL;
    length = PAGE_ALIGN_UP(length);
    uint64_t va = (flags & MAP_FIXED) && addr ? PAGE_ALIGN_DOWN(addr)
                                              : (p->mmap_bump += length, p->mmap_bump - length);
    uint64_t vf = VMM_UDATA;
    if (!(prot & PROT_WRITE))
        vf &= ~(uint64_t) VMM_WRITE;
    for (uint64_t o = 0; o < length; o += PAGE_SIZE)
    {
        void* ph = pmm_alloc_zeroed();
        if (!ph)
            return -(int64_t) ENOMEM;
        if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0)
        {
            pmm_free(ph);
            return -(int64_t) ENOMEM;
        }
    }
    return (int64_t) va;
}

static int64_t sys_munmap(uint64_t addr, uint64_t len)
{
    proc_t* p = cur();
    if (!p)
        return -(int64_t) EINVAL;
    for (uint64_t o = 0; o < PAGE_ALIGN_UP(len); o += PAGE_SIZE)
        vmm_unmap(p->space, addr + o);
    return 0;
}

static int64_t sys_mprotect(uint64_t a, uint64_t l, uint64_t f)
{
    (void) a;
    (void) l;
    (void) f;
    return 0;
}

static int64_t sys_fork(syscall_frame_t* f)
{
    proc_t* parent = cur();
    if (!parent)
        return -(int64_t) ENOMEM;

    proc_t* child = proc_alloc(parent->pid);
    if (!child)
        return -(int64_t) ENOMEM;

    child->space = vmm_space_new();
    if (!child->space)
        goto fail_space;

    if (vmm_fork_user(child->space, parent->space) < 0)
        goto fail_fork;

    vfs_copy_fdtable(child->fds, parent->fds);

    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->user_rsp = cpu_get_user_rsp();

    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = parent->fs_base;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    uint8_t* ksp = child->kstack + KSTACK_SIZE;

    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t* cf = (syscall_frame_t*) ksp;
    *cf = *f;
    cf->rax = 0;

    ksp -= 8;
    *(uint64_t*) ksp = (uint64_t) (uintptr_t) proc_resume_frame;

    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);

    child->kstack_rsp = (uint64_t) ksp;
    child->state = PROC_READY;

    log_info("[fork] parent=%u child=%u", parent->pid, child->pid);
    return (int64_t) child->pid;

fail_fork:
    vmm_space_free(child->space);
fail_space:
    kfree(child->kstack);
    kfree(child->fds);
    child->state = PROC_UNUSED;
    return -(int64_t) ENOMEM;
}

#define MAX_EXEC_ARGS 32

static int64_t sys_execve(const char* path, const char** uargv, const char** uenvp)
{
    if (!path)
        return -(int64_t) EFAULT;

    /* Copy argv/envp strings from user space to kernel heap BEFORE switching
     * address spaces. The old user space stays mapped until vmm_switch.     */
    char* argv_mem[MAX_EXEC_ARGS] = {0};
    char* envp_mem[MAX_EXEC_ARGS] = {0};
    const char* kargv[MAX_EXEC_ARGS + 1];
    const char* kenvp[MAX_EXEC_ARGS + 1];
    int argc = 0, envc = 0;

#define FREE_EXEC_STRS()                                                                           \
    do                                                                                             \
    {                                                                                              \
        for (int _i = 0; _i < MAX_EXEC_ARGS; _i++)                                                 \
        {                                                                                          \
            if (argv_mem[_i])                                                                      \
            {                                                                                      \
                kfree(argv_mem[_i]);                                                               \
                argv_mem[_i] = NULL;                                                               \
            }                                                                                      \
            if (envp_mem[_i])                                                                      \
            {                                                                                      \
                kfree(envp_mem[_i]);                                                               \
                envp_mem[_i] = NULL;                                                               \
            }                                                                                      \
        }                                                                                          \
    } while (0)

    if (uargv)
    {
        while (argc < MAX_EXEC_ARGS && uargv[argc])
        {
            size_t n = strlen(uargv[argc]) + 1;
            argv_mem[argc] = kmalloc(n);
            if (!argv_mem[argc])
            {
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(argv_mem[argc], uargv[argc], n);
            kargv[argc] = argv_mem[argc];
            argc++;
        }
    }
    if (argc == 0)
    {
        size_t n = strlen(path) + 1;
        argv_mem[0] = kmalloc(n);
        if (!argv_mem[0])
        {
            FREE_EXEC_STRS();
            return -(int64_t) ENOMEM;
        }
        memcpy(argv_mem[0], path, n);
        kargv[0] = argv_mem[0];
        argc = 1;
    }
    kargv[argc] = NULL;

    if (uenvp)
    {
        while (envc < MAX_EXEC_ARGS && uenvp[envc])
        {
            size_t n = strlen(uenvp[envc]) + 1;
            envp_mem[envc] = kmalloc(n);
            if (!envp_mem[envc])
            {
                FREE_EXEC_STRS();
                return -(int64_t) ENOMEM;
            }
            memcpy(envp_mem[envc], uenvp[envc], n);
            kenvp[envc] = envp_mem[envc];
            envc++;
        }
    }
    kenvp[envc] = NULL;

    vfs_node_t* node = vfs_lookup(path);
    if (!node || node->type != VFS_TYPE_REG || !node->data)
    {
        FREE_EXEC_STRS();
        return -(int64_t) ENOENT;
    }

    elf_load_result_t res;
    if (elf_load(node->data, node->size, &res) < 0)
    {
        FREE_EXEC_STRS();
        return -(int64_t) ENOEXEC;
    }

    uint64_t rsp = setup_user_stack(res.space, &res, argc, (const char* const*) kargv,
                                    (const char* const*) kenvp);
    FREE_EXEC_STRS();

    if (!rsp)
    {
        vmm_space_free(res.space);
        return -(int64_t) ENOMEM;
    }

    proc_t* p = cur();
    vmm_space_t* old = p->space;

    p->space = res.space;
    p->brk = PAGE_ALIGN_UP(res.brk);
    p->brk_base = p->brk;
    p->mmap_bump = 0x0000500000000000ULL;
    g_current_space = p->space;

    vmm_switch(p->space);
    vmm_space_free(old);

    for (int i = 0; i < NSIG; i++)
    {
        if (p->sig_actions[i].sa_handler != SIG_IGN)
        {
            p->sig_actions[i].sa_handler = SIG_DFL;
            p->sig_actions[i].sa_flags = 0;
            p->sig_actions[i].sa_restorer = 0;
            p->sig_actions[i].sa_mask = 0;
        }
    }

    log_info("[exec] pid=%u entry=0x%lx rsp=0x%lx", p->pid, res.entry, rsp);
    enter_userspace_exec(res.entry, rsp, 0x202ULL);
}

__attribute__((noreturn)) void proc_do_exit(int code)
{
    proc_t* p = cur();
    log_info("[pid %u] exit(%d)", p->pid, code);
    vfs_free_fdtable(p->fds);
    p->fds = NULL;

    p->exit_code = code;
    p->state = PROC_ZOMBIE;

    proc_t* parent = proc_find(p->ppid);
    if (parent && parent->state != PROC_UNUSED && parent->state != PROC_RUNNING)
    {
        proc_send_signal(parent, SIGCHLD);
        parent->state = PROC_READY;
        vfs_set_fdtable(parent->fds);
        g_current_space = parent->space;
        cpu_set_kernel_stack(parent->kstack_top);
        sched_switch(parent);
    }
    cpu_halt();
}

static int64_t sys_wait4(int pid, int* wstatus, int options, void* rusage)
{
    (void) options;
    (void) rusage;
    proc_t* parent = cur();

    while (1)
    {
        bool any_child = false;
        for (int i = 0; i < PROC_MAX; i++)
        {
            proc_t* c = &g_proctable[i];
            if (c->state == PROC_UNUSED)
                continue;
            if (c->ppid != parent->pid)
                continue;
            if (pid > 0 && (int) c->pid != pid)
                continue;
            any_child = true;

            if (c->state == PROC_ZOMBIE)
            {
                if (wstatus)
                    *wstatus = (c->exit_code & 0xFF) << 8;
                uint32_t cpid = c->pid;
                if (c->fds)
                    vfs_free_fdtable(c->fds);
                vmm_space_free(c->space);
                kfree(c->kstack);
                memset(c, 0, sizeof(*c));
                c->state = PROC_UNUSED;
                return (int64_t) cpid;
            }
        }

        if (!any_child)
            return -(int64_t) ECHILD;

        proc_t* next = NULL;
        for (int i = 0; i < PROC_MAX; i++)
        {
            proc_t* c = &g_proctable[i];
            if (c->state != PROC_READY)
                continue;
            if (c->ppid != parent->pid)
                continue;
            if (pid > 0 && (int) c->pid != pid)
                continue;
            next = c;
            break;
        }

        if (!next)
            return -(int64_t) ECHILD;

        parent->state = PROC_WAITING;
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        sched_switch(next);

        parent->state = PROC_RUNNING;
        vfs_set_fdtable(parent->fds);
        g_current_space = parent->space;
        cpu_set_kernel_stack(parent->kstack_top);
    }
}

#define ARCH_SET_FS 0x1002
#define ARCH_SET_GS 0x1001
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

static int64_t sys_arch_prctl(int code, uint64_t addr)
{
    switch (code)
    {
    case ARCH_SET_FS:
        wrmsr(0xC0000100, addr);
        cur()->fs_base = addr;
        return 0;
    case ARCH_SET_GS:
        wrmsr(0xC0000101, addr);
        return 0;
    case ARCH_GET_FS:
        return (int64_t) cur()->fs_base;
    case ARCH_GET_GS:
        return (int64_t) rdmsr(0xC0000101);
    }
    return -(int64_t) EINVAL;
}

struct utsname
{
    char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};

static int64_t sys_uname(struct utsname* buf)
{
    if (!buf)
        return -(int64_t) EFAULT;
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->sysname, "Kyronix", 7);
    memcpy(buf->nodename, "localhost", 9);
    memcpy(buf->release, "0.0.1", 5);
    memcpy(buf->version, "#1 SMP", 6);
    memcpy(buf->machine, "x86_64", 6);
    return 0;
}

static int64_t sys_getcwd(char* buf, uint64_t size)
{
    if (!buf || !size)
        return -(int64_t) EINVAL;
    proc_t* p = cur();
    const char* cwd = p ? p->cwd : g_cwd;
    size_t len = strlen(cwd) + 1;
    if (len > size)
        return -(int64_t) EINVAL;
    memcpy(buf, cwd, len);
    return (int64_t) (uintptr_t) buf;
}

static int64_t sys_chdir(const char* path)
{
    if (!path)
        return -(int64_t) EFAULT;
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int64_t) ENOENT;
    if (n->type != VFS_TYPE_DIR)
        return -(int64_t) ENOTDIR;
    proc_t* p = cur();
    if (p)
        strncpy(p->cwd, path, sizeof(p->cwd) - 1);
    return 0;
}

static int64_t sys_getpid(void)
{
    return cur() ? (int64_t) cur()->pid : 1;
}
static int64_t sys_getppid(void)
{
    return cur() ? (int64_t) cur()->ppid : 0;
}
static int64_t sys_getuid(void)
{
    return 0;
}
static int64_t sys_getgid(void)
{
    return 0;
}
static int64_t sys_geteuid(void)
{
    return 0;
}
static int64_t sys_getegid(void)
{
    return 0;
}
static int64_t sys_getpgrp(void)
{
    return cur() ? (int64_t) cur()->pgid : 1;
}
static int64_t sys_getpgid(uint64_t pid)
{
    if (pid == 0)
        return sys_getpgrp();
    proc_t* p = proc_find((uint32_t) pid);
    return p ? (int64_t) p->pgid : -(int64_t) ESRCH;
}
static int64_t sys_setsid(void)
{
    proc_t* p = cur();
    if (!p)
        return -(int64_t) EPERM;
    p->pgid = p->pid;
    return (int64_t) p->pid;
}
static int64_t sys_setpgid(uint64_t pid, uint64_t pgid)
{
    proc_t* p = pid ? proc_find((uint32_t) pid) : cur();
    if (!p)
        return -(int64_t) ESRCH;
    if (pgid == 0)
        pgid = p->pid;
    if (pgid > PROC_MAX)
        return -(int64_t) EINVAL;
    p->pgid = (int) pgid;
    return 0;
}
static int64_t sys_kill(int64_t pid, int sig)
{
    if (sig == 0)
        return 0;
    if (sig < 0 || sig >= NSIG)
        return -(int64_t) EINVAL;

    if (pid > 0)
    {
        proc_t* target = proc_find((uint32_t) pid);
        if (!target)
            return -(int64_t) ESRCH;
        proc_send_signal(target, sig);
    }
    else if (pid == -1)
    {
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (g_proctable[i].state == PROC_UNUSED)
                continue;
            if (g_proctable[i].pid == 1)
                continue;
            proc_send_signal(&g_proctable[i], sig);
        }
    }
    else if (pid < -1)
    {
        uint32_t pgid = (uint32_t) (-pid);
        bool found = false;
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (g_proctable[i].state == PROC_UNUSED)
                continue;
            if (g_proctable[i].pgid == (int) pgid)
            {
                proc_send_signal(&g_proctable[i], sig);
                found = true;
            }
        }
        if (!found)
            return -(int64_t) ESRCH;
    }
    else
    {
        proc_t* self = cur();
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (g_proctable[i].state == PROC_UNUSED)
                continue;
            if (g_proctable[i].pgid == self->pgid)
                proc_send_signal(&g_proctable[i], sig);
        }
    }
    return 0;
}

static int64_t sys_getrlimit(uint64_t r, void* rl)
{
    (void) r;
    if (!rl)
        return -(int64_t) EINVAL;
    ((uint64_t*) rl)[0] = (uint64_t) -1;
    ((uint64_t*) rl)[1] = (uint64_t) -1;
    return 0;
}

static int64_t sys_prlimit64(uint64_t p, uint64_t r, void* nl, void* ol)
{
    (void) p;
    (void) r;
    (void) nl;
    if (ol)
    {
        ((uint64_t*) ol)[0] = (uint64_t) -1;
        ((uint64_t*) ol)[1] = (uint64_t) -1;
    }
    return 0;
}

static int64_t sys_rt_sigaction(int sig, const k_sigaction_t* act, k_sigaction_t* oldact,
                                uint64_t sigsetsize)
{
    (void) sigsetsize;
    if (sig < 1 || sig >= NSIG)
        return -(int64_t) EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -(int64_t) EINVAL;

    proc_t* p = cur();
    if (oldact)
        memcpy(oldact, &p->sig_actions[sig - 1], sizeof(k_sigaction_t));
    if (act)
        memcpy(&p->sig_actions[sig - 1], act, sizeof(k_sigaction_t));
    return 0;
}

static int64_t sys_rt_sigprocmask(int how, const uint64_t* set, uint64_t* oldset,
                                  uint64_t sigsetsize)
{
    (void) sigsetsize;
    proc_t* p = cur();

    if (oldset)
        *oldset = p->sig_mask;
    if (!set)
        return 0;

    uint64_t bits = *set & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    switch (how)
    {
    case SIG_BLOCK:
        p->sig_mask |= bits;
        break;
    case SIG_UNBLOCK:
        p->sig_mask &= ~bits;
        break;
    case SIG_SETMASK:
        p->sig_mask = bits;
        break;
    default:
        return -(int64_t) EINVAL;
    }
    return 0;
}

static int64_t sys_rt_sigreturn(syscall_frame_t* f)
{
    rt_sigframe_t* frame = (rt_sigframe_t*) (cpu_get_user_rsp() - 8);
    mcontext_t* mc = &frame->uc.uc_mcontext;

    f->r8 = mc->r8;
    f->r9 = mc->r9;
    f->r10 = mc->r10;
    f->r11 = mc->eflags; /* r11 = user RFLAGS (restored via sysretq) */
    f->r12 = mc->r12;
    f->r13 = mc->r13;
    f->r14 = mc->r14;
    f->r15 = mc->r15;
    f->rdi = mc->rdi;
    f->rsi = mc->rsi;
    f->rbp = mc->rbp;
    f->rbx = mc->rbx;
    f->rdx = mc->rdx;
    f->rcx = mc->rip; /* user RIP (restored via sysretq RCX->RIP) */

    cpu_set_user_rsp(mc->rsp);

    proc_t* p = cur();
    p->sig_mask = frame->uc.uc_sigmask;
    p->sig_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    return (int64_t) mc->rax;
}

static int64_t sys_pause(void)
{
    proc_t* p = cur();
    if (!p)
        return 0;
    while (!(p->pending_sigs & ~p->sig_mask))
    {
        proc_t* next = NULL;
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (&g_proctable[i] == p)
                continue;
            if (g_proctable[i].state == PROC_READY)
            {
                next = &g_proctable[i];
                break;
            }
        }
        if (!next)
        {
            cpu_set_kernel_stack(p->kstack_top);
            sti();
            hlt();
            continue;
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
    return -(int64_t) EINTR;
}

static int64_t sys_nanosleep(void* r, void* m)
{
    (void) m;
    if (!r) return 0;
    uint64_t sec = ((uint64_t*) r)[0];
    uint64_t nsec = ((uint64_t*) r)[1];
    uint64_t ms = sec * 1000 + nsec / 1000000;
    if (ms == 0) return 0;
    proc_t* p = cur();
    if (!p) return 0;
    uint64_t deadline = g_ticks + ms;
    p->wakeup_tick = deadline;
    while (g_ticks < deadline) {
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
    }
    p->wakeup_tick = 0;
    return 0;
}
static void path_abs(char* out, const char* in)
{
    if (!in || in[0] == '/') {
        strncpy(out, in ? in : "", 511);
        out[511] = '\0';
        return;
    }
    proc_t* p = cur();
    const char* cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t cl = strlen(cwd);
    if (cl >= 511) { out[0] = '/'; out[1] = '\0'; return; }
    memcpy(out, cwd, cl);
    if (out[cl - 1] != '/') out[cl++] = '/';
    strncpy(out + cl, in, 511 - cl);
    out[511] = '\0';
}

static int64_t sys_mkdir(const char* path, uint32_t mode)
{
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    path_abs(abs, path);
    return (int64_t) vfs_mkdir(abs, mode);
}

static int64_t sys_rmdir(const char* path)
{
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    path_abs(abs, path);
    return (int64_t) vfs_rmdir(abs);
}

static int64_t sys_unlink(const char* path)
{
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    path_abs(abs, path);
    return (int64_t) vfs_unlink(abs);
}

static int64_t sys_rename(const char* oldpath, const char* newpath)
{
    if (!oldpath || !newpath) return -(int64_t) EINVAL;
    char abs_old[512], abs_new[512];
    path_abs(abs_old, oldpath);
    path_abs(abs_new, newpath);
    return (int64_t) vfs_rename(abs_old, abs_new);
}

static int64_t sys_set_tid_address(void* p)
{
    (void) p;
    return sys_getpid();
}
static int64_t sys_set_robust_list(void* h, uint64_t l)
{
    (void) h;
    (void) l;
    return 0;
}

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 128
static int64_t sys_futex(uint32_t* uaddr, int op, uint32_t val, void* timeout, uint32_t* uaddr2,
                         uint32_t val3)
{
    (void) timeout;
    (void) uaddr2;
    (void) val3;
    if (!uaddr)
        return -(int64_t) EFAULT;
    int cmd = op & ~FUTEX_PRIVATE_FLAG;
    switch (cmd)
    {
    case FUTEX_WAIT:
        if (*uaddr != val)
            return -(int64_t) EAGAIN;
        return 0;
    case FUTEX_WAKE:
        return 0;
    default:
        return -(int64_t) ENOSYS;
    }
}

static int64_t sys_getrandom(void* buf, uint64_t len, uint32_t flags)
{
    (void) flags;
    if (!buf || !len)
        return 0;
    uint8_t* p = (uint8_t*) buf;
    for (uint64_t i = 0; i < len; i++)
        p[i] = (uint8_t) (0x53 ^ (i * 0x6b + 0x37));
    return (int64_t) len;
}

static int64_t sys_sigaltstack(const void* ss, void* oss)
{
    (void) ss;
    (void) oss;
    return 0;
}

static int64_t sys_tgkill(int tgid, int tid, int sig)
{
    (void) tgid;
    return sys_kill((int64_t) tid, sig);
}

static int64_t sys_gettid(void)
{
    return sys_getpid();
}

static int64_t sys_prctl(int op, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void) op;
    (void) a2;
    (void) a3;
    (void) a4;
    (void) a5;
    return 0;
}

struct pollfd_s
{
    int fd;
    short events;
    short revents;
};

#define POLLIN 1
#define POLLOUT 4

static int64_t sys_poll(struct pollfd_s* fds, uint64_t nfds, int timeout)
{
    uint64_t iterations = 0;
    uint64_t max_iterations = (timeout < 0) ? UINT64_MAX
                            : (uint64_t) (timeout > 0 ? timeout : 0);

    for (;;)
    {
        uint64_t ready = 0;
        for (uint64_t i = 0; i < nfds; i++)
        {
            fds[i].revents = 0;

            if (fds[i].events & POLLIN)
            {
                if (fds[i].fd < 0 || fd_pollin(fds[i].fd))
                    fds[i].revents |= POLLIN;
            }
            if (fds[i].events & POLLOUT)
            {
                if (fds[i].fd < 0 || fd_pollout(fds[i].fd))
                    fds[i].revents |= POLLOUT;
            }

            if (fds[i].revents)
                ready++;
        }

        if (ready > 0)
            return (int64_t) ready;

        if (timeout == 0)
            return 0;

        iterations++;
        if (iterations >= max_iterations)
            return 0;

        if (g_current_proc && (g_current_proc->pending_sigs & ~g_current_proc->sig_mask))
            return -(int64_t) EINTR;

        sched_yield_blocking();
        cpu_relax();
    }
}

static int64_t sys_ppoll(struct pollfd_s* fds, uint64_t nfds, void* tmo, const void* sigmask,
                         uint64_t sigsetsize)
{
    int timeout = -1;
    if (tmo)
    {
        uint64_t* ts = (uint64_t*) tmo;
        timeout = (int)(ts[0] * 1000 + ts[1] / 1000000);
    }
    (void) sigmask;
    (void) sigsetsize;
    return sys_poll(fds, nfds, timeout);
}

static int64_t sys_select(int nfds, void* rfds, void* wfds, void* efds, void* timeout)
{
    (void) nfds;
    (void) rfds;
    (void) wfds;
    (void) efds;
    (void) timeout;
    return 0;
}

static int64_t sys_pselect6(int nfds, void* rfds, void* wfds, void* efds, void* timeout,
                            void* sigmask)
{
    (void) nfds;
    (void) rfds;
    (void) wfds;
    (void) efds;
    (void) timeout;
    (void) sigmask;
    return 0;
}

static int64_t sys_umask(uint64_t mask)
{
    (void) mask;
    return 0022;
}

static int64_t sys_ftruncate(int fd, uint64_t len)
{
    (void) fd;
    (void) len;
    return 0;
}

static int64_t sys_statfs(const char* path, void* buf)
{
    (void) path;
    if (buf)
        memset(buf, 0, 120);
    return 0;
}

static int64_t sys_getgroups(int size, uint32_t* list)
{
    (void) size;
    (void) list;
    return 0;
}

static int64_t sys_setgroups(int size, const uint32_t* list)
{
    (void) size;
    (void) list;
    return 0;
}

static int64_t sys_madvise(void* addr, uint64_t len, int advice)
{
    (void) addr;
    (void) len;
    (void) advice;
    return 0;
}

static int64_t sys_getitimer(int w, void* v)
{
    (void) w;
    if (v)
        memset(v, 0, 32);
    return 0;
}
static int64_t sys_setitimer(int w, const void* n, void* o)
{
    (void) w;
    (void) n;
    if (o)
        memset(o, 0, 32);
    return 0;
}

static int64_t sys_clock_gettime(uint64_t c, void* t)
{
    (void) c;
    if (t)
    {
        uint64_t ms = g_ticks;
        ((uint64_t*) t)[0] = ms / 1000;
        ((uint64_t*) t)[1] = (ms % 1000) * 1000000ULL;
    }
    return 0;
}

static int64_t sys_gettimeofday(void* tv, void* tz)
{
    (void) tz;
    if (tv)
    {
        uint64_t ms = g_ticks;
        ((uint64_t*) tv)[0] = ms / 1000;
        ((uint64_t*) tv)[1] = (ms % 1000) * 1000ULL;
    }
    return 0;
}

static int64_t sys_times(void* b)
{
    if (b)
        memset(b, 0, 32);
    return 0;
}

struct iovec
{
    uint64_t iov_base;
    uint64_t iov_len;
};

static int64_t sys_readv(int fd, const struct iovec* iov, int n)
{
    int64_t total = 0;
    for (int i = 0; i < n; i++)
    {
        int64_t r = fd_read(fd, (void*) iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
        {
            if (!total)
                total = r;
            break;
        }
        total += r;
        if ((uint64_t) r < iov[i].iov_len)
            break;
    }
    return total;
}

static int64_t sys_writev(int fd, const void* iov_ptr, int n)
{
    const struct iovec* iov = (const struct iovec*) iov_ptr;
    int64_t total = 0;
    for (int i = 0; i < n; i++)
    {
        int64_t r = fd_write(fd, (const void*) iov[i].iov_base, iov[i].iov_len);
        if (r < 0)
        {
            if (!total)
                total = r;
            break;
        }
        total += r;
    }
    return total;
}

static int64_t sys_access(const char* p, int m)
{
    (void) m;
    return vfs_lookup(p) ? 0 : -(int64_t) ENOENT;
}

void syscall_dispatch(syscall_frame_t* f)
{
    uint64_t nr = f->rax;
    uint64_t a1 = f->rdi, a2 = f->rsi, a3 = f->rdx;
    uint64_t a4 = f->r10, a5 = f->r8, a6 = f->r9;

    int64_t ret;
    switch (nr)
    {
    case 0:
        ret = fd_read((int) a1, (void*) a2, a3);
        break;
    case 1:
        ret = fd_write((int) a1, (const void*) a2, a3);
        break;
    case 2:
        ret = fd_open((const char*) a1, (int) a2, (int) a3);
        break;
    case 3:
        ret = fd_close((int) a1);
        break;
    case 4:
        ret = fd_stat((const char*) a1, (struct linux_stat*) a2);
        break;
    case 5:
        ret = fd_fstat((int) a1, (struct linux_stat*) a2);
        break;
    case 6:
        ret = fd_lstat((const char*) a1, (struct linux_stat*) a2);
        break;
    case 7:
        ret = sys_poll((struct pollfd_s*) a1, a2, (int) a3);
        break;
    case 8:
        ret = fd_lseek((int) a1, (int64_t) a2, (int) a3);
        break;
    case 9:
        ret = sys_mmap(a1, a2, a3, a4, a5, a6);
        break;
    case 10:
        ret = sys_mprotect(a1, a2, a3);
        break;
    case 11:
        ret = sys_munmap(a1, a2);
        break;
    case 12:
        ret = sys_brk(a1);
        break;
    case 13:
        ret = sys_rt_sigaction((int) a1, (const k_sigaction_t*) a2, (k_sigaction_t*) a3, a4);
        break;
    case 14:
        ret = sys_rt_sigprocmask((int) a1, (const uint64_t*) a2, (uint64_t*) a3, a4);
        break;
    case 15:
        ret = sys_rt_sigreturn(f);
        break;
    case 16:
        ret = fd_ioctl((int) a1, a2, a3);
        break;
    case 17:
    {
        int64_t s = fd_lseek((int) a1, (int64_t) a4, SEEK_SET);
        ret = (s < 0) ? s : fd_read((int) a1, (void*) a2, a3);
        break;
    }
    case 19:
        ret = sys_readv((int) a1, (const struct iovec*) a2, (int) a3);
        break;
    case 20:
        ret = sys_writev((int) a1, (const void*) a2, (int) a3);
        break;
    case 21:
        ret = sys_access((const char*) a1, (int) a2);
        break;
    case 22:
        ret = fd_pipe((int*) a1);
        break;
    case 23:
        ret = sys_select((int) a1, (void*) a2, (void*) a3, (void*) a4, (void*) a5);
        break;
    case 28:
        ret = sys_madvise((void*) a1, a2, (int) a3);
        break;
    case 32:
        ret = fd_dup((int) a1);
        break;
    case 33:
        ret = fd_dup2((int) a1, (int) a2);
        break;
    case 34:
        ret = sys_pause();
        break;
    case 35:
        ret = sys_nanosleep((void*) a1, (void*) a2);
        break;
    case 36:
        ret = sys_getitimer((int) a1, (void*) a2);
        break;
    case 38:
        ret = sys_setitimer((int) a1, (const void*) a2, (void*) a3);
        break;
    case 39:
        ret = sys_getpid();
        break;
    case 57:
        ret = sys_fork(f);
        break;
    case 59:
        ret = sys_execve((const char*) a1, (const char**) a2, (const char**) a3);
        break;
    case 60:
        proc_do_exit((int) a1);
        return;
    case 61:
        ret = sys_wait4((int) a1, (int*) a2, (int) a3, (void*) a4);
        break;
    case 62:
        ret = sys_kill(a1, (int) a2);
        break;
    case 63:
        ret = sys_uname((struct utsname*) a1);
        break;
    case 72:
        ret = fd_fcntl((int) a1, (int) a2, a3);
        break;
    case 77:
        ret = sys_ftruncate((int) a1, a2);
        break;
    case 78:
        ret = fd_getdents64((int) a1, (void*) a2, a3);
        break;
    case 79:
        ret = sys_getcwd((char*) a1, a2);
        break;
    case 80:
        ret = sys_chdir((const char*) a1);
        break;
    case 82:
        ret = sys_rename((const char*) a1, (const char*) a2);
        break;
    case 83:
        ret = sys_mkdir((const char*) a1, (uint32_t) a2);
        break;
    case 84:
        ret = sys_rmdir((const char*) a1);
        break;
    case 87:
        ret = sys_unlink((const char*) a1);
        break;
    case 89:
        ret = fd_readlink((const char*) a1, (char*) a2, a3);
        break;
    case 95:
        ret = sys_umask(a1);
        break;
    case 96:
        ret = sys_gettimeofday((void*) a1, (void*) a2);
        break;
    case 97:
        ret = sys_getrlimit(a1, (void*) a2);
        break;
    case 100:
        ret = sys_times((void*) a1);
        break;
    case 102:
        ret = sys_getuid();
        break;
    case 104:
        ret = sys_getgid();
        break;
    case 105:
        ret = sys_setgroups((int) a1, (uint32_t*) a2);
        break;
    case 107:
        ret = sys_geteuid();
        break;
    case 108:
        ret = sys_getegid();
        break;
    case 109:
        ret = sys_setpgid(a1, a2);
        break;
    case 110:
        ret = sys_getppid();
        break;
    case 111:
        ret = sys_getpgrp();
        break;
    case 112:
        ret = sys_setsid();
        break;
    case 115:
        ret = sys_getgroups((int) a1, (uint32_t*) a2);
        break;
    case 121:
        ret = sys_getpgid(a1);
        break;
    case 131:
        ret = sys_sigaltstack((const void*) a1, (void*) a2);
        break;
    case 137:
        ret = sys_statfs((const char*) a1, (void*) a2);
        break;
    case 138:
        ret = sys_statfs(NULL, (void*) a2);
        break;
    case 157:
        ret = sys_prctl((int) a1, a2, a3, a4, a5);
        break;
    case 158:
        ret = sys_arch_prctl((int) a1, a2);
        break;
    case 160:
        ret = sys_getrlimit(a1, (void*) a2);
        break;
    case 186:
        ret = sys_gettid();
        break;
    case 200:
        ret = sys_kill((int64_t) a1, (int) a2);
        break; /* tkill */
    case 202:
        ret = sys_futex((uint32_t*) a1, (int) a2, (uint32_t) a3, (void*) a4, (uint32_t*) a5,
                        (uint32_t) a6);
        break;
    case 217:
        ret = fd_getdents64((int) a1, (void*) a2, a3);
        break;
    case 218:
        ret = sys_set_tid_address((void*) a1);
        break;
    case 228:
        ret = sys_clock_gettime(a1, (void*) a2);
        break;
    case 229:
        ret = 0;
        break;
    case 231:
        proc_do_exit((int) a1);
        return;
    case 234:
        ret = sys_tgkill((int) a1, (int) a2, (int) a3);
        break;
    case 257:
        ret = fd_openat((int) a1, (const char*) a2, (int) a3, (int) a4);
        break;
    case 258:
        ret = sys_mkdir((const char*) a2, (uint32_t) a3);
        break;
    case 262:
        ret = fd_fstatat((int) a1, (const char*) a2, (struct linux_stat*) a3, (int) a4);
        break;
    case 263:
        if ((int) a3 & 0x200)
            ret = sys_rmdir((const char*) a2);
        else
            ret = sys_unlink((const char*) a2);
        break;
    case 270:
        ret = sys_pselect6((int) a1, (void*) a2, (void*) a3, (void*) a4, (void*) a5, (void*) a6);
        break;
    case 271:
        ret = sys_ppoll((struct pollfd_s*) a1, a2, (void*) a3, (const void*) a4, a5);
        break;
    case 273:
        ret = sys_set_robust_list((void*) a1, a2);
        break;
    case 292:
        ret = fd_dup2((int) a1, (int) a2);
        break;
    case 293:
        ret = fd_pipe((int*) a1);
        break;
    case 302:
        ret = sys_prlimit64(a1, a2, (void*) a3, (void*) a4);
        break;
    case 318:
        ret = sys_getrandom((void*) a1, a2, (uint32_t) a3);
        break;

    default:
        log_debug("[syscall %lu  a1=%lx a2=%lx a3=%lx]", nr, a1, a2, a3);
        ret = -(int64_t) ENOSYS;
        break;
    }
    f->rax = (uint64_t) ret;

    signal_check(f);
}
