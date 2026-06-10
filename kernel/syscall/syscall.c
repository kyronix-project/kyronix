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
#define EACCES 13
#define EEXIST 17
#define ENOSPC 28
#define ECHILD 10
#define EAGAIN 11
#define ENOMEM 12
#define EFAULT 14
#define ENOEXEC 8
#define ENOTDIR 20
#define EINVAL 22
#define EMFILE 24
#define ENOSYS 38
#define EISDIR    21
#define ENOSPC    28
#define ETIMEDOUT 110
#define ENOTEMPTY 39
#define ENAMETOOLONG 36
#define ENOTSUP  95
#define ELOOP    40
#define EOVERFLOW 75

vmm_space_t* g_current_space = NULL;

static void cleartid_wake(uint32_t* addr);

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
#define PROT_EXEC 0x4
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
    if (!length)
        return -(int64_t) EINVAL;
    length = PAGE_ALIGN_UP(length);
    uint64_t va = (flags & MAP_FIXED) && addr ? PAGE_ALIGN_DOWN(addr)
                                              : (p->mmap_bump += length, p->mmap_bump - length);
    uint64_t vf = VMM_UDATA;
    if (!(prot & PROT_WRITE))
        vf &= ~(uint64_t) VMM_WRITE;

    if (!(flags & MAP_ANON))
    {
        /* file-backed: MAP_PRIVATE — allocate pages and copy file content */
        vfs_node_t* fn = fd_get_node((int) fd);
        if (!fn || fn->type != VFS_TYPE_REG)
            return -(int64_t) EBADF;
        uint64_t file_size = fn->size;
        for (uint64_t o = 0; o < length; o += PAGE_SIZE)
        {
            void* ph = pmm_alloc_zeroed();
            if (!ph) return -(int64_t) ENOMEM;
            if (vmm_map(p->space, va + o, (uint64_t) ph, vf) < 0)
            {
                pmm_free(ph);
                return -(int64_t) ENOMEM;
            }
            if (fn->data && off + o < file_size)
            {
                uint64_t copy = file_size - (off + o);
                if (copy > PAGE_SIZE) copy = PAGE_SIZE;
                memcpy(phys_to_virt((uint64_t) ph), fn->data + off + o, copy);
            }
        }
        return (int64_t) va;
    }

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
    if (!uptr_ok((void*) addr, len)) return -(int64_t) EINVAL;
    proc_t* p = cur();
    if (!p)
        return -(int64_t) EINVAL;
    for (uint64_t o = 0; o < PAGE_ALIGN_UP(len); o += PAGE_SIZE)
        vmm_unmap(p->space, addr + o);
    return 0;
}

static int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot)
{
    proc_t* p = cur();
    if (!p || !len) return 0;
    addr = PAGE_ALIGN_DOWN(addr);
    len  = PAGE_ALIGN_UP(len);
    uint64_t flags = VMM_USER | VMM_NX;
    if (prot & PROT_WRITE) flags |= VMM_WRITE;
    if (prot & PROT_EXEC)  flags &= ~(uint64_t) VMM_NX;
    for (uint64_t o = 0; o < len; o += PAGE_SIZE)
        vmm_protect(p->space, addr + o, flags);
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
    child->uid = parent->uid; child->euid = parent->euid; child->suid = parent->suid;
    child->gid = parent->gid; child->egid = parent->egid; child->sgid = parent->sgid;
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
    proc_kstack_free(child);
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

    if (p->is_thread)
    {
        if (p->cleartid_addr)
        {
            *p->cleartid_addr = 0;
            cleartid_wake(p->cleartid_addr);
        }
        proc_kstack_free(p);
        memset(p, 0, sizeof(*p));
        p->state = PROC_UNUSED;
        cpu_halt();
    }

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
                proc_kstack_free(c);
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
    memcpy(buf->nodename, "kx", 2);
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

static void path_abs(char*, const char*);

static int64_t sys_chdir(const char* path)
{
    if (!path) return -(int64_t)EFAULT;
    char abs[512];
    path_abs(abs, path);
    const char* rp = abs[0] ? abs : path;
    vfs_node_t* n = vfs_lookup(rp);
    if (!n) return -(int64_t)ENOENT;
    if (n->type != VFS_TYPE_DIR) return -(int64_t)ENOTDIR;
    proc_t* p = cur();
    if (p) strncpy(p->cwd, rp, sizeof(p->cwd) - 1);
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
static int64_t sys_getuid(void)  { proc_t* p = cur(); return p ? (int64_t) p->uid  : 0; }
static int64_t sys_getgid(void)  { proc_t* p = cur(); return p ? (int64_t) p->gid  : 0; }
static int64_t sys_geteuid(void) { proc_t* p = cur(); return p ? (int64_t) p->euid : 0; }
static int64_t sys_getegid(void) { proc_t* p = cur(); return p ? (int64_t) p->egid : 0; }

static int64_t sys_setuid(uint32_t uid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid == 0) { p->uid = p->euid = p->suid = uid; return 0; }
    if (uid != p->uid && uid != p->suid) return -(int64_t) EPERM;
    p->euid = uid;
    return 0;
}
static int64_t sys_setgid(uint32_t gid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid == 0) { p->gid = p->egid = p->sgid = gid; return 0; }
    if (gid != p->gid && gid != p->sgid) return -(int64_t) EPERM;
    p->egid = gid;
    return 0;
}
static int64_t sys_setreuid(uint32_t ruid, uint32_t euid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (ruid != (uint32_t)-1 && ruid != p->uid && ruid != p->euid))
        return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (euid != (uint32_t)-1 && euid != p->uid && euid != p->euid && euid != p->suid))
        return -(int64_t) EPERM;
    if (ruid != (uint32_t)-1) p->uid  = ruid;
    if (euid != (uint32_t)-1) p->euid = euid;
    p->suid = p->euid;
    return 0;
}
static int64_t sys_setregid(uint32_t rgid, uint32_t egid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (rgid != (uint32_t)-1 && rgid != p->gid && rgid != p->egid))
        return -(int64_t) EPERM;
    if (p->euid != 0 &&
        (egid != (uint32_t)-1 && egid != p->gid && egid != p->egid && egid != p->sgid))
        return -(int64_t) EPERM;
    if (rgid != (uint32_t)-1) p->gid  = rgid;
    if (egid != (uint32_t)-1) p->egid = egid;
    p->sgid = p->egid;
    return 0;
}
static int64_t sys_setresuid(uint32_t ruid, uint32_t euid, uint32_t suid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0)
    {
        uint32_t cur_set[3] = {p->uid, p->euid, p->suid};
        if (ruid != (uint32_t)-1 && ruid != cur_set[0] && ruid != cur_set[1] && ruid != cur_set[2]) return -(int64_t) EPERM;
        if (euid != (uint32_t)-1 && euid != cur_set[0] && euid != cur_set[1] && euid != cur_set[2]) return -(int64_t) EPERM;
        if (suid != (uint32_t)-1 && suid != cur_set[0] && suid != cur_set[1] && suid != cur_set[2]) return -(int64_t) EPERM;
    }
    if (ruid != (uint32_t)-1) p->uid  = ruid;
    if (euid != (uint32_t)-1) p->euid = euid;
    if (suid != (uint32_t)-1) p->suid = suid;
    return 0;
}
static int64_t sys_setresgid(uint32_t rgid, uint32_t egid, uint32_t sgid)
{
    proc_t* p = cur();
    if (!p) return -(int64_t) EPERM;
    if (p->euid != 0)
    {
        uint32_t cur_set[3] = {p->gid, p->egid, p->sgid};
        if (rgid != (uint32_t)-1 && rgid != cur_set[0] && rgid != cur_set[1] && rgid != cur_set[2]) return -(int64_t) EPERM;
        if (egid != (uint32_t)-1 && egid != cur_set[0] && egid != cur_set[1] && egid != cur_set[2]) return -(int64_t) EPERM;
        if (sgid != (uint32_t)-1 && sgid != cur_set[0] && sgid != cur_set[1] && sgid != cur_set[2]) return -(int64_t) EPERM;
    }
    if (rgid != (uint32_t)-1) p->gid  = rgid;
    if (egid != (uint32_t)-1) p->egid = egid;
    if (sgid != (uint32_t)-1) p->sgid = sgid;
    return 0;
}
static int64_t sys_getresuid(uint32_t* ruid, uint32_t* euid, uint32_t* suid)
{
    proc_t* p = cur();
    if (!p || !uptr_ok(ruid, 4) || !uptr_ok(euid, 4) || !uptr_ok(suid, 4)) return -(int64_t) EFAULT;
    *ruid = p->uid; *euid = p->euid; *suid = p->suid;
    return 0;
}
static int64_t sys_getresgid(uint32_t* rgid, uint32_t* egid, uint32_t* sgid)
{
    proc_t* p = cur();
    if (!p || !uptr_ok(rgid, 4) || !uptr_ok(egid, 4) || !uptr_ok(sgid, 4)) return -(int64_t) EFAULT;
    *rgid = p->gid; *egid = p->egid; *sgid = p->sgid;
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
static bool kill_permitted(const proc_t* sender, const proc_t* target)
{
    if (!sender) return true; /* kernel */
    if (sender->euid == 0) return true;
    return sender->uid == target->uid || sender->uid == target->euid ||
           sender->euid == target->uid || sender->euid == target->euid;
}

static int64_t sys_kill(int64_t pid, int sig)
{
    if (sig == 0)
        return 0;
    if (sig < 0 || sig >= NSIG)
        return -(int64_t) EINVAL;

    proc_t* self = cur();

    if (pid > 0)
    {
        proc_t* target = proc_find((uint32_t) pid);
        if (!target)
            return -(int64_t) ESRCH;
        if (!kill_permitted(self, target))
            return -(int64_t) EPERM;
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
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (g_proctable[i].state == PROC_UNUSED)
                continue;
            if (self && g_proctable[i].pgid == self->pgid)
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

static int64_t sys_rt_sigsuspend(const uint64_t* mask, uint64_t sigsetsize)
{
    (void) sigsetsize;
    proc_t* p = cur();
    if (!p) return 0;
    uint64_t saved = p->sig_mask;
    if (mask)
    {
        p->sig_mask = *mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }
    while (!(p->pending_sigs & ~p->sig_mask))
    {
        proc_t* next = NULL;
        for (int i = 0; i < PROC_MAX; i++)
        {
            if (&g_proctable[i] == p) continue;
            if (g_proctable[i].state == PROC_READY) { next = &g_proctable[i]; break; }
        }
        if (!next)
        {
            cpu_set_kernel_stack(p->kstack_top);
            sti(); hlt();
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
    p->sig_mask = saved;
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
    proc_t* proc = cur();
    if (proc) proc->cleartid_addr = (uint32_t*) p;
    return (int64_t) sys_getpid();
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
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_MAX_WAITERS PROC_MAX

typedef struct {
    uint32_t* uaddr;
    proc_t*   proc;
} futex_entry_t;

static futex_entry_t g_futex_tab[FUTEX_MAX_WAITERS];

static void cleartid_wake(uint32_t* addr)
{
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
    {
        if (g_futex_tab[i].uaddr == addr && g_futex_tab[i].proc)
        {
            g_futex_tab[i].proc->state = PROC_READY;
            g_futex_tab[i].proc = NULL;
        }
    }
}

static int64_t sys_futex(uint32_t* uaddr, int op, uint32_t val, void* timeout, uint32_t* uaddr2,
                         uint32_t val3)
{
    (void) uaddr2;
    (void) val3;
    if (!uaddr || !uptr_ok(uaddr, sizeof(*uaddr)))
        return -(int64_t) EFAULT;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    switch (cmd)
    {
    case FUTEX_WAIT:
    {
        if (*uaddr != val)
            return -(int64_t) EAGAIN;
        proc_t* p = cur();
        if (!p) return -(int64_t) EFAULT;
        int slot = -1;
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
            if (!g_futex_tab[i].proc) { slot = i; break; }
        if (slot < 0) return -(int64_t) ENOMEM;
        g_futex_tab[slot].uaddr = uaddr;
        g_futex_tab[slot].proc  = p;
        uint64_t deadline = 0;
        if (timeout) {
            uint64_t ms = ((uint64_t*) timeout)[0] * 1000 + ((uint64_t*) timeout)[1] / 1000000;
            if (ms) deadline = g_ticks + ms;
        }
        p->wakeup_tick = deadline;
        while (g_futex_tab[slot].proc == p) {
            if (deadline && g_ticks >= deadline) break;
            if (proc_next_ready(p))
                sched_yield_blocking();
            else {
                sti();
                hlt();
                cli();
            }
        }
        bool timed_out = deadline && g_ticks >= deadline;
        p->wakeup_tick = 0;
        if (g_futex_tab[slot].proc == p)
            g_futex_tab[slot].proc = NULL;
        return timed_out ? -(int64_t) ETIMEDOUT : 0;
    }
    case FUTEX_WAKE:
    {
        int woken = 0;
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int) val; i++) {
            if (g_futex_tab[i].uaddr == uaddr && g_futex_tab[i].proc) {
                g_futex_tab[i].proc->wakeup_tick = 0;
                if (g_futex_tab[i].proc->state == PROC_WAITING)
                    g_futex_tab[i].proc->state = PROC_READY;
                g_futex_tab[i].proc = NULL;
                woken++;
            }
        }
        return (int64_t) woken;
    }
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

#define POLLIN   0x0001
#define POLLOUT  0x0004
#define POLLHUP  0x0010
#define POLLNVAL 0x0020

struct pollfd_s
{
    int fd;
    short events;
    short revents;
};

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

static inline bool fds_test(uint8_t* set, int fd)
{
    return set && (set[fd >> 3] & (1 << (fd & 7)));
}
static inline void fds_set(uint8_t* set, int fd)
{
    if (set) set[fd >> 3] |= (1 << (fd & 7));
}

static int64_t sys_select(int nfds, void* rfds, void* wfds, void* efds, void* timeout)
{
    (void) efds;
    uint64_t deadline = (uint64_t) -1ULL;
    if (timeout) {
        uint64_t ms = ((uint64_t*) timeout)[0] * 1000 + ((uint64_t*) timeout)[1] / 1000;
        deadline = g_ticks + ms;
    }
    uint8_t rout[128], wout[128];
    proc_t* p = cur();
    for (;;) {
        memset(rout, 0, sizeof(rout));
        memset(wout, 0, sizeof(wout));
        int ready = 0;
        for (int fd = 0; fd < nfds && fd < VFS_FD_MAX; fd++) {
            if (fds_test((uint8_t*) rfds, fd) && fd_pollin(fd))  { fds_set(rout, fd); ready++; }
            if (fds_test((uint8_t*) wfds, fd) && fd_pollout(fd)) { fds_set(wout, fd); ready++; }
        }
        if (ready > 0 || g_ticks >= deadline) {
            if (rfds) memcpy(rfds, rout, sizeof(rout));
            if (wfds) memcpy(wfds, wout, sizeof(wout));
            return (int64_t) ready;
        }
        if (p && (p->pending_sigs & ~p->sig_mask)) return -(int64_t) EINTR;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else { sti(); hlt(); cli(); }
    }
}

static int64_t sys_pselect6(int nfds, void* rfds, void* wfds, void* efds, void* timeout,
                            void* sigmask)
{
    (void) sigmask;
    /* pselect uses timespec (nsec), convert to timeval-style (usec) for sys_select */
    uint64_t tv[2] = {0, 0};
    if (timeout) {
        tv[0] = ((uint64_t*) timeout)[0];
        tv[1] = ((uint64_t*) timeout)[1] / 1000;
    }
    return sys_select(nfds, rfds, wfds, efds, timeout ? tv : NULL);
}

static int64_t sys_umask(uint64_t mask)
{
    (void) mask;
    return 0022;
}

static int64_t sys_ftruncate(int fd, uint64_t len)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n) return -(int64_t) EBADF;
    if (n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (len < n->size) n->size = len;
    return 0;
}

static int64_t sys_truncate(const char* path, uint64_t len)
{
    if (!path) return -(int64_t) EFAULT;
    char abs[512];
    path_abs(abs, path);
    vfs_node_t* n = vfs_lookup(abs[0] ? abs : path);
    if (!n) return -(int64_t) ENOENT;
    if (n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (len < n->size) n->size = len;
    return 0;
}

static int64_t sys_symlink(const char* target, const char* linkpath)
{
    if (!target || !linkpath) return -(int64_t) EFAULT;
    char abs[512];
    path_abs(abs, linkpath);
    vfs_node_t* n = vfs_create_symlink(abs[0] ? abs : linkpath, target);
    return n ? 0 : -(int64_t) EEXIST;
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
        uint64_t ms = g_epoch_base * 1000 + g_ticks;
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
        uint64_t ms = g_epoch_base * 1000 + g_ticks;
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
    (void)m;
    if (!p) return -(int64_t)EFAULT;
    char abs[512];
    path_abs(abs, p);
    return vfs_lookup(abs[0] ? abs : p) ? 0 : -(int64_t)ENOENT;
}

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

static int64_t sys_mremap(uint64_t old_addr, uint64_t old_sz, uint64_t new_sz,
                          uint64_t flags, uint64_t new_addr)
{
    proc_t* p = cur();
    if (!p || !old_sz || !new_sz) return -(int64_t)EINVAL;
    old_sz = PAGE_ALIGN_UP(old_sz);
    new_sz = PAGE_ALIGN_UP(new_sz);
    if (new_sz <= old_sz) {
        for (uint64_t o = new_sz; o < old_sz; o += PAGE_SIZE) {
            uint64_t ph = vmm_virt_to_phys(p->space, old_addr + o);
            vmm_unmap(p->space, old_addr + o);
            if (ph) pmm_free((void*)ph);
        }
        return (int64_t)old_addr;
    }
    if (!(flags & MREMAP_MAYMOVE)) return -(int64_t)ENOMEM;
    uint64_t new_va;
    if (flags & MREMAP_FIXED) new_va = PAGE_ALIGN_DOWN(new_addr);
    else { p->mmap_bump += new_sz; new_va = p->mmap_bump - new_sz; }
    for (uint64_t o = 0; o < new_sz; o += PAGE_SIZE) {
        void* ph = pmm_alloc_zeroed();
        if (!ph) return -(int64_t)ENOMEM;
        if (vmm_map(p->space, new_va + o, (uint64_t)ph, VMM_UDATA) < 0) { pmm_free(ph); return -(int64_t)ENOMEM; }
        if (o < old_sz) {
            uint64_t src = vmm_virt_to_phys(p->space, old_addr + o);
            if (src) memcpy(phys_to_virt((uint64_t)ph), phys_to_virt(src), PAGE_SIZE);
        }
    }
    for (uint64_t o = 0; o < old_sz; o += PAGE_SIZE) {
        uint64_t ph = vmm_virt_to_phys(p->space, old_addr + o);
        vmm_unmap(p->space, old_addr + o);
        if (ph) pmm_free((void*)ph);
    }
    return (int64_t)new_va;
}
static int64_t sys_sendfile(int outfd, int infd, uint64_t* offp, uint64_t count)
{
    vfs_file_t* inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG) return -(int64_t)EINVAL;
    vfs_node_t* src = inf->node;
    uint64_t off = offp ? *offp : inf->pos;
    if (off >= src->size) return 0;
    uint64_t avail = src->size - off;
    if (count > avail) count = avail;
    int64_t w = fd_write(outfd, src->data + off, count);
    if (w > 0) { if (offp) *offp = off + (uint64_t)w; else inf->pos = off + (uint64_t)w; }
    return w;
}

static int64_t sys_preadv(int fd, const struct iovec* iov, int n, uint64_t off)
{
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_pread(fd, (void*)iov[i].iov_base, iov[i].iov_len, off + (uint64_t)total);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
    }
    return total;
}
static int64_t sys_pwritev(int fd, const struct iovec* iov, int n, uint64_t off)
{
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_pwrite(fd, (const void*)iov[i].iov_base, iov[i].iov_len, off + (uint64_t)total);
        if (r < 0) return total ? total : r;
        total += r;
    }
    return total;
}

static uint32_t g_memfd_seq;
static int64_t sys_memfd_create(const char* name, uint32_t flags)
{
    (void)name; (void)flags;
    char path[32];
    snprintf(path, sizeof(path), "/tmp/.mfd%u", ++g_memfd_seq);
    return fd_open(path, O_RDWR|O_CREAT|O_TRUNC, 0600);
}

static int64_t sys_copy_file_range(int infd, uint64_t* off_in, int outfd, uint64_t* off_out,
                                   uint64_t len, uint32_t flags)
{
    (void)flags;
    vfs_file_t* inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG) return -(int64_t)EINVAL;
    vfs_node_t* src = inf->node;
    uint64_t rin = off_in ? *off_in : inf->pos;
    if (rin >= src->size) return 0;
    uint64_t avail = src->size - rin;
    if (len > avail) len = avail;
    uint64_t rout = off_out ? *off_out : (fd_get_file(outfd) ? fd_get_file(outfd)->pos : 0);
    int64_t w = fd_pwrite(outfd, src->data + rin, len, rout);
    if (w > 0) {
        if (off_in)  *off_in  = rin  + (uint64_t)w; else inf->pos = rin + (uint64_t)w;
        if (off_out) *off_out = rout + (uint64_t)w;
        else if (fd_get_file(outfd)) fd_get_file(outfd)->pos = rout + (uint64_t)w;
    }
    return w;
}

#define STATX_BASIC_STATS 0x7ffU

struct statx_ts { int64_t tv_sec; uint32_t tv_nsec; int32_t _r; };
struct statx {
    uint32_t stx_mask, stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink, stx_uid, stx_gid;
    uint16_t stx_mode; uint16_t _pad0;
    uint64_t stx_ino, stx_size, stx_blocks, stx_attributes_mask;
    struct statx_ts stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major, stx_rdev_minor, stx_dev_major, stx_dev_minor;
    uint64_t stx_mnt_id, _spare[9];
};

static int64_t sys_statx(int dirfd, const char* path, int flags, uint32_t mask,
                         struct statx* sx)
{
    (void)mask;
    if (!sx) return -(int64_t)EFAULT;
    vfs_node_t* n = NULL;
    if (!path || path[0] == '\0') {
        n = fd_get_node(dirfd);
    } else {
        char abs[512];
        at_resolve(dirfd, path, abs, sizeof(abs));
        n = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(abs) : vfs_lookup(abs);
    }
    if (!n) return -(int64_t)ENOENT;
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask    = STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink   = 1;
    sx->stx_uid     = n->uid; sx->stx_gid = n->gid;
    sx->stx_mode    = (uint16_t)n->mode;
    sx->stx_ino     = n->ino;
    sx->stx_size    = n->size;
    sx->stx_blocks  = (n->size + 511) / 512;
    sx->stx_dev_major = 1;
    return 0;
}

static int64_t sys_rt_sigtimedwait(const uint64_t* set, void* info, const void* timeout,
                                   uint64_t sigsetsize)
{
    (void)sigsetsize; (void)info;
    proc_t* p = cur();
    if (!p || !set) return -(int64_t)EFAULT;
    uint64_t mask = *set;
    uint64_t deadline = (uint64_t)-1ULL;
    if (timeout) {
        uint64_t ms = ((const uint64_t*)timeout)[0] * 1000 + ((const uint64_t*)timeout)[1] / 1000000;
        if (ms) deadline = g_ticks + ms;
    }
    while (!(p->pending_sigs & mask)) {
        if (deadline != (uint64_t)-1ULL && g_ticks >= deadline) return -(int64_t)ETIMEDOUT;
        if (proc_next_ready(p)) sched_yield_blocking();
        else { sti(); hlt(); cli(); }
    }
    uint64_t bit = p->pending_sigs & mask;
    int sig = 1;
    while (!((bit >> (sig-1)) & 1)) sig++;
    p->pending_sigs &= ~(1ULL << (sig-1));
    return (int64_t)sig;
}

#define EPOLLIN    0x001U
#define EPOLLOUT   0x004U
#define EPOLLERR   0x008U
#define EPOLLHUP   0x010U
#define EPOLLET    (1U<<31)
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_SLOTS 32
#define EPOLL_MAXW  64

struct epoll_event { uint32_t events; uint64_t data; } __attribute__((packed));
struct epoll_watch { int fd; uint32_t events; uint64_t data; };
typedef struct { int epfd; struct epoll_watch w[EPOLL_MAXW]; int nw; } epoll_t;
static epoll_t g_epolls[EPOLL_SLOTS];
static int g_epoll_init;

static epoll_t* epoll_find(int epfd)
{
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == epfd) return &g_epolls[i];
    return NULL;
}

static int64_t sys_epoll_create1(int flags)
{
    (void)flags;
    if (!g_epoll_init) {
        for (int i = 0; i < EPOLL_SLOTS; i++) g_epolls[i].epfd = -1;
        g_epoll_init = 1;
    }
    /* reclaim slots whose fd was closed */
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd >= 0 && !fd_valid(g_epolls[i].epfd))
            g_epolls[i].epfd = -1;
    int epfd = fd_open("/dev/null", O_RDONLY, 0);
    if (epfd < 0) return -(int64_t)EMFILE;
    epoll_t* ep = NULL;
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == -1) { ep = &g_epolls[i]; break; }
    if (!ep) { fd_close(epfd); return -(int64_t)ENOMEM; }
    ep->epfd = epfd; ep->nw = 0;
    return epfd;
}

static int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev)
{
    epoll_t* ep = epoll_find(epfd);
    if (!ep) return -(int64_t)EBADF;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (ep->nw >= EPOLL_MAXW) return -(int64_t)ENOMEM;
        for (int i = 0; i < ep->nw; i++) if (ep->w[i].fd == fd) return -(int64_t)EEXIST;
        ep->w[ep->nw] = (struct epoll_watch){ fd, ev ? ev->events : EPOLLIN|EPOLLOUT, ev ? ev->data : 0 };
        ep->nw++;
        return 0;
    case EPOLL_CTL_DEL:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) { ep->w[i] = ep->w[--ep->nw]; return 0; }
        return -(int64_t)ENOENT;
    case EPOLL_CTL_MOD:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) { ep->w[i].events = ev ? ev->events : 0; ep->w[i].data = ev ? ev->data : 0; return 0; }
        return -(int64_t)ENOENT;
    }
    return -(int64_t)EINVAL;
}

static int64_t sys_epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
    epoll_t* ep = epoll_find(epfd);
    if (!ep || !events || maxevents <= 0) return -(int64_t)EINVAL;
    proc_t* p = cur();
    uint64_t deadline = timeout >= 0 ? g_ticks + (uint64_t)timeout : (uint64_t)-1ULL;
    for (;;) {
        int n = 0;
        for (int i = 0; i < ep->nw && n < maxevents; i++) {
            int wfd = ep->w[i].fd;
            uint32_t got = fd_valid(wfd) ? 0 : (EPOLLERR|EPOLLHUP);
            if (!got) {
                if ((ep->w[i].events & EPOLLIN)  && fd_pollin(wfd))  got |= EPOLLIN;
                if ((ep->w[i].events & EPOLLOUT) && fd_pollout(wfd)) got |= EPOLLOUT;
            }
            if (got) { events[n].events = got; events[n].data = ep->w[i].data; n++; }
        }
        if (n > 0 || timeout == 0 || g_ticks >= deadline) return n;
        if (p && (p->pending_sigs & ~p->sig_mask)) return -(int64_t)EINTR;
        if (proc_next_ready(p)) sched_yield_blocking();
        else { sti(); hlt(); cli(); }
    }
}

static int64_t sys_alarm(uint32_t seconds)
{
    proc_t* p = cur();
    if (!p) return 0;
    uint64_t prev = 0;
    if (p->alarm_tick && p->alarm_tick > g_ticks)
        prev = (p->alarm_tick - g_ticks + 999) / 1000;
    p->alarm_tick = seconds ? g_ticks + (uint64_t)seconds * 1000 : 0;
    return (int64_t)prev;
}

static int64_t sys_fchdir(int fd)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n) return -(int64_t)EBADF;
    if (n->type != VFS_TYPE_DIR) return -(int64_t)ENOTDIR;
    proc_t* p = cur();
    if (!p) return 0;
    char buf[512];
    if (vfs_node_abspath(n, buf, sizeof(buf)))
        strncpy(p->cwd, buf, sizeof(p->cwd) - 1);
    return 0;
}

static int64_t sys_link(const char* old, const char* lnew)
{
    if (!old || !lnew) return -(int64_t)EFAULT;
    char abs_old[512], abs_new[512];
    path_abs(abs_old, old); path_abs(abs_new, lnew);
    return (int64_t)vfs_link(abs_old, abs_new);
}

static int64_t sys_clock_getres(uint64_t clk, void* res)
{
    (void)clk;
    if (res) { ((uint64_t*)res)[0] = 0; ((uint64_t*)res)[1] = 1000000ULL; }
    return 0;
}

struct sysinfo_s {
    int64_t  uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram;
    uint64_t totalswap, freeswap;
    uint16_t procs;
    uint8_t  _pad[6];
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
};

static int64_t sys_sysinfo(struct sysinfo_s* si)
{
    if (!si) return -(int64_t)EFAULT;
    memset(si, 0, sizeof(*si));
    si->uptime   = (int64_t)(g_ticks / 1000);
    si->totalram = 256ULL * 1024 * 1024;
    si->freeram  = 128ULL * 1024 * 1024;
    si->mem_unit = 1;
    si->procs    = 1;
    return 0;
}

#define CLONE_VM      0x00000100
#define CLONE_THREAD  0x00010000

static int64_t sys_clone(uint64_t flags, uint64_t rsp, uint32_t* parent_tid, uint32_t* child_tid,
                         uint64_t tls, syscall_frame_t* f)
{
    proc_t* parent = cur();
    if (!parent)
        return -(int64_t) ENOMEM;

    proc_t* child = proc_alloc(parent->pid);
    if (!child)
        return -(int64_t) ENOMEM;

    if (flags & CLONE_VM)
    {
        child->space = parent->space;
    }
    else
    {
        child->space = vmm_space_new();
        if (!child->space) goto fail_space;
        if (vmm_fork_user(child->space, parent->space) < 0) goto fail_fork;
    }

    vfs_copy_fdtable(child->fds, parent->fds);
    child->brk = parent->brk;
    child->brk_base = parent->brk_base;
    child->mmap_bump = parent->mmap_bump;
    child->pgid = parent->pgid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->sig_mask = parent->sig_mask;
    memcpy(child->sig_actions, parent->sig_actions, sizeof(parent->sig_actions));
    child->pending_sigs = 0;
    child->fs_base = parent->fs_base;
    child->is_thread = (flags & CLONE_THREAD) ? true : false;
    memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    uint8_t* ksp = child->kstack + KSTACK_SIZE;

    ksp -= sizeof(syscall_frame_t);
    syscall_frame_t* cf = (syscall_frame_t*) ksp;
    *cf = *f;
    cf->rax = 0;
    if (rsp)
        child->user_rsp = rsp;
    else
        child->user_rsp = parent->user_rsp;

    ksp -= 8;
    *(uint64_t*) ksp = (uint64_t) (uintptr_t) proc_resume_frame;

    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);

    if (tls && (flags & CLONE_VM))
        child->fs_base = tls;
    if (parent_tid) *parent_tid = 0;
    if (child_tid) *child_tid = child->pid;

    child->kstack_rsp = (uint64_t) ksp;
    child->state = PROC_READY;

    log_info("[clone] parent=%u child=%u flags=0x%lx", parent->pid, child->pid, flags);
    return (int64_t) child->pid;

fail_fork:
    vmm_space_free(child->space);
fail_space:
    kfree(child->kstack);
    kfree(child->fds);
    child->state = PROC_UNUSED;
    return -(int64_t) ENOMEM;
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
    case 25:
        ret = sys_mremap(a1, a2, a3, a4, a5);
        break;
    case 26:
        ret = 0; /* msync: no-op (ramfs) */
        break;
    case 27: /* mincore: report all pages resident */
        if (a3) memset((void*)a3, 1, (a2 + PAGE_SIZE - 1) / PAGE_SIZE);
        ret = 0;
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
        ret = fd_pread((int)a1, (void*)a2, a3, a4);
        break;
    case 18:
        ret = fd_pwrite((int)a1, (const void*)a2, a3, a4);
        break;
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
    case 24:
        sched_yield_blocking();
        ret = 0;
        break;
    case 53:
        ret = fd_socketpair((int*) a4);
        break;
    case 23:
        ret = sys_select((int) a1, (void*) a2, (void*) a3, (void*) a4, (void*) a5);
        break;
    case 28:
        ret = sys_madvise((void*) a1, a2, (int) a3);
        break;
    case 29: case 30: case 31:
        ret = -(int64_t)ENOSYS; /* shmget/shmat/shmctl */
        break;
    case 40:
        ret = sys_sendfile((int)a1, (int)a2, (uint64_t*)a3, a4);
        break;
    case 41: case 42: case 43: case 44: case 45: case 46: case 47:
    case 48: case 49: case 50: case 51: case 52:
        ret = -(int64_t)ENOSYS; /* socket group: no network stack */
        break;
    case 54:
        ret = 0; /* setsockopt: accept silently */
        break;
    case 55:
        ret = -(int64_t)EINVAL; /* getsockopt */
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
        ret = sys_nanosleep((void*)a1, (void*)a2);
        break;
    case 36:
        ret = sys_getitimer((int)a1, (void*)a2);
        break;
    case 37:
        ret = sys_alarm((uint32_t)a1);
        break;
    case 38:
        ret = sys_setitimer((int) a1, (const void*) a2, (void*) a3);
        break;
    case 39:
        ret = sys_getpid();
        break;
    case 56:
        ret = sys_clone(a1, a2, (uint32_t*)a3, (uint32_t*)a4, a5, f);
        break;
    case 57:
        ret = sys_fork(f);
        break;
    case 58:
        ret = sys_fork(f); /* vfork: treat as fork */
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
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71:
        ret = -(int64_t)ENOSYS; /* SysV IPC: im too lazy now  */
        break;
    case 72:
        ret = fd_fcntl((int)a1, (int)a2, a3);
        break;
    case 73:
        ret = fd_valid((int)a1) ? 0 : -(int64_t)EBADF;
        break;
    case 74: case 75:
        ret = fd_valid((int)a1) ? 0 : -(int64_t)EBADF;
        break;
    case 76:
        ret = sys_truncate((const char*) a1, a2);
        break;
    case 77:
        ret = sys_ftruncate((int) a1, a2);
        break;
    case 78:
        ret = fd_getdents64((int) a1, (void*) a2, a3);
        break;
    case 79:
        ret = sys_getcwd((char*)a1, a2);
        break;
    case 80:
        ret = sys_chdir((const char*)a1);
        break;
    case 81:
        ret = sys_fchdir((int)a1);
        break;
    case 82:
        ret = sys_rename((const char*) a1, (const char*) a2);
        break;
    case 83:
        ret = sys_mkdir((const char*) a1, (uint32_t) a2);
        break;
    case 84:
        ret = sys_rmdir((const char*)a1);
        break;
    case 85:
        ret = fd_open((const char*)a1, O_WRONLY|O_CREAT|O_TRUNC, (int)a2);
        break;
    case 86:
        ret = sys_link((const char*)a1, (const char*)a2);
        break;
    case 87:
        ret = sys_unlink((const char*)a1);
        break;
    case 88:
        ret = sys_symlink((const char*) a1, (const char*) a2);
        break;
    case 89:
        ret = fd_readlink((const char*)a1, (char*)a2, a3);
        break;
    case 90: { char abs[512]; path_abs(abs, (const char*)a1); ret = (int64_t)vfs_chmod(abs[0] ? abs : (const char*)a1, (uint32_t)a2); break; }
    case 91:
        ret = (int64_t)vfs_fchmod((int)a1, (uint32_t)a2);
        break;
    case 92: { char abs[512]; path_abs(abs, (const char*)a1); ret = (int64_t)vfs_chown(abs[0] ? abs : (const char*)a1, (uint32_t)a2, (uint32_t)a3); break; }
    case 93:
        ret = (int64_t)vfs_fchown((int)a1, (uint32_t)a2, (uint32_t)a3);
        break;
    case 94: { char abs[512]; path_abs(abs, (const char*)a1); ret = (int64_t)vfs_lchown(abs[0] ? abs : (const char*)a1, (uint32_t)a2, (uint32_t)a3); break; }
    case 95:
        ret = sys_umask(a1);
        break;
    case 96:
        ret = sys_gettimeofday((void*) a1, (void*) a2);
        break;
    case 97:
        ret = sys_getrlimit(a1, (void*)a2);
        break;
    case 98:
        if (a2) memset((void*)a2, 0, 144);
        ret = 0;
        break;
    case 99:
        ret = sys_sysinfo((struct sysinfo_s*)a1);
        break;
    case 100:
        ret = sys_times((void*)a1);
        break;
    case 101:
        ret = -(int64_t)EPERM; /* ptrace */
        break;
    case 103:
        ret = -(int64_t)EPERM; /* syslog */
        break;
    case 102:
        ret = sys_getuid();
        break;
    case 104:
        ret = sys_getgid();
        break;
    case 105:
        ret = sys_setuid((uint32_t) a1);
        break;
    case 106:
        ret = sys_setgid((uint32_t) a1);
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
    case 113:
        ret = sys_setreuid((uint32_t) a1, (uint32_t) a2);
        break;
    case 114:
        ret = sys_setregid((uint32_t) a1, (uint32_t) a2);
        break;
    case 115:
        ret = sys_getgroups((int) a1, (uint32_t*) a2);
        break;
    case 116:
        ret = sys_setgroups((int) a1, (uint32_t*) a2);
        break;
    case 117:
        ret = sys_setresuid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 118:
        ret = sys_getresuid((uint32_t*) a1, (uint32_t*) a2, (uint32_t*) a3);
        break;
    case 119:
        ret = sys_setresgid((uint32_t) a1, (uint32_t) a2, (uint32_t) a3);
        break;
    case 120:
        ret = sys_getresgid((uint32_t*) a1, (uint32_t*) a2, (uint32_t*) a3);
        break;
    case 121:
        ret = sys_getpgid(a1);
        break;
    case 122: { proc_t* _p = cur(); ret = _p ? (int64_t)_p->euid : 0; if (_p && a1 != (uint64_t)-1) _p->euid = (uint32_t)a1; break; }
    case 123: { proc_t* _p = cur(); ret = _p ? (int64_t)_p->egid : 0; if (_p && a1 != (uint64_t)-1) _p->egid = (uint32_t)a1; break; }
    case 124: { proc_t* _p = a1 ? proc_find((uint32_t)a1) : cur(); ret = _p ? (int64_t)_p->pgid : -(int64_t)ESRCH; break; }
    case 125: if (a2) memset((void*)a2, 0, 40); ret = 0; break; /* capget: no caps */
    case 126: ret = 0; break; /* capset */
    case 127: { if (a1) *(uint64_t*)a1 = cur() ? (cur()->pending_sigs & cur()->sig_mask) : 0; ret = 0; break; }
    case 128: ret = sys_rt_sigtimedwait((const uint64_t*)a1, (void*)a2, (const void*)a3, a4); break;
    case 129: ret = 0; break; /* rt_sigqueueinfo */
    case 132: ret = 0; break; /* utime: no-op */
    case 130:
        ret = sys_rt_sigsuspend((const uint64_t*) a1, a2);
        break;
    case 131:
        ret = sys_sigaltstack((const void*) a1, (void*) a2);
        break;
    case 133: { char abs[512]; if (!a1) { ret = -(int64_t)EFAULT; break; } path_abs(abs, (const char*)a1); ret = (int64_t)vfs_mknod(abs[0] ? abs : (const char*)a1, (uint32_t)a2, a3); break; }
    case 135: ret = 0; break; /* personality */
    case 139: ret = -(int64_t)ENOSYS; break; /* sysfs */
    case 140: ret = 0; break; /* getpriority */
    case 141: ret = 0; break; /* setpriority */
    case 142: ret = 0; break; /* sched_setparam */
    case 143: if (a2) ((int*)a2)[0] = 0; ret = 0; break; /* sched_getparam */
    case 144: ret = 0; break; /* sched_setscheduler */
    case 145: ret = 0; break; /* sched_getscheduler: SCHED_OTHER */
    case 146: ret = 0; break; /* sched_get_priority_max */
    case 147: ret = 0; break; /* sched_get_priority_min */
    case 148: if (a2) { ((uint64_t*)a2)[0]=0; ((uint64_t*)a2)[1]=10000000ULL; } ret = 0; break; /* sched_rr_get_interval */
    case 149: case 150: case 151: case 152: ret = 0; break; /* mlock/munlock */
    case 153: ret = 0; break; /* vhangup */
    case 137:
        ret = sys_statfs((const char*)a1, (void*)a2);
        break;
    case 138:
        ret = sys_statfs(NULL, (void*) a2);
        break;
    case 157:
        ret = sys_prctl((int)a1, a2, a3, a4, a5);
        break;
    case 158:
        ret = sys_arch_prctl((int)a1, a2);
        break;
    case 159: ret = 0; break; /* adjtimex */
    case 160:
        ret = sys_getrlimit(a1, (void*)a2); /* setrlimit: accept silently */
        break;
    case 161: ret = 0; break; /* chroot: no-op */
    case 162:
        ret = 0; /* sync: no-op (ramfs) */
        break;
    case 169:
        cpu_halt();
        ret = 0;
        break;
    case 164: ret = 0; break; /* settimeofday */
    case 170:
        ret = 0; /* sethostname */
        break;
    case 171: ret = 0; break; /* setdomainname */
    case 172: case 173: ret = -(int64_t)EPERM; break; /* iopl, ioperm */
    case 175: case 176: ret = -(int64_t)ENOSYS; break; /* init/delete_module */
    case 188: case 189: case 190: case 191: case 192: case 193:
    case 194: case 195: case 196: case 197: case 198: case 199:
        ret = -(int64_t)ENOTSUP; /* xattr: not supported */
        break;
    case 186:
        ret = sys_gettid();
        break;
    case 201: { uint64_t t = g_epoch_base + g_ticks / 1000; if (a1) *(uint64_t*)a1 = t; ret = (int64_t)t; break; }
    case 203: ret = 0; break; /* sched_setaffinity */
    case 204: { uint64_t sz = a2; if (a3 && sz > 0) { memset((void*)a3, 0, sz); if (sz >= 1) *(uint8_t*)a3 = 1; } ret = 0; break; } /* sched_getaffinity: 1 CPU */
    case 213: ret = sys_epoll_create1(0); break; /* epoll_create */
    case 221: ret = sys_epoll_wait((int)a1, (struct epoll_event*)a2, (int)a3, -1); break; /* epoll_wait (old) */
    case 222: case 224: case 226: ret = 0; break; /* timer_create/gettime/delete stubs */
    case 223: ret = 0; break; /* timer_settime */
    case 225: ret = 0; break; /* timer_getoverrun */
    case 227: ret = 0; break; /* clock_settime */
    case 232: ret = sys_epoll_wait((int)a1, (struct epoll_event*)a2, (int)a3, (int)a4); break;
    case 233: ret = sys_epoll_ctl((int)a1, (int)a2, (int)a3, (struct epoll_event*)a4); break;
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
        ret = sys_clock_getres(a1, (void*)a2);
        break;
    case 230:
        ret = sys_nanosleep((void*)a3, (void*)a4); /* clock_nanosleep: ignore clkid/flags */
        break;
    case 231:
        proc_do_exit((int) a1);
        return;
    case 234:
        ret = sys_tgkill((int)a1, (int)a2, (int)a3);
        break;
    case 235:
        ret = 0; /* utimes */
        break;
    case 236: ret = -(int64_t)ENOSYS; break; /* vserver */
    case 240: case 241: case 242: case 243: case 244: case 245:
        ret = -(int64_t)ENOSYS; break; /* mqueue */
    case 251: case 252: ret = 0; break; /* ioprio_set/get */
    case 253: case 254: case 255: ret = -(int64_t)ENOSYS; break; /* inotify */
    case 247: /* waitid */
        ret = sys_wait4((int)a2, NULL, (int)a4, NULL);
        break;
    case 257:
        ret = fd_openat((int) a1, (const char*) a2, (int) a3, (int) a4);
        break;
    case 258: { char abs[512]; at_resolve((int)a1, (const char*)a2, abs, sizeof(abs)); ret = (int64_t)vfs_mkdir(abs, (uint32_t)a3); break; }
    case 260: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); ret=(int64_t)vfs_mknod(abs,(uint32_t)a3,a4); break; } /* mknodat */
    case 261: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); int _r=(int)a5&AT_SYMLINK_NOFOLLOW?(int)vfs_lchown(abs,(uint32_t)a3,(uint32_t)a4):(int)vfs_chown(abs,(uint32_t)a3,(uint32_t)a4); ret=_r; break; } /* fchownat */
    case 262:
        ret = fd_fstatat((int)a1, (const char*)a2, (struct linux_stat*)a3, (int)a4);
        break;
    case 263: { char abs[512]; at_resolve((int)a1, (const char*)a2, abs, sizeof(abs)); ret = ((int)a3 & 0x200) ? (int64_t)vfs_rmdir(abs) : (int64_t)vfs_unlink(abs); break; }
    case 264: { char ao[512], an[512]; at_resolve((int)a1,(const char*)a2,ao,sizeof(ao)); at_resolve((int)a3,(const char*)a4,an,sizeof(an)); ret=(int64_t)vfs_rename(ao,an); break; }
    case 265: { char ao[512], an[512]; at_resolve((int)a1,(const char*)a2,ao,sizeof(ao)); at_resolve((int)a3,(const char*)a4,an,sizeof(an)); ret=(int64_t)vfs_link(ao,an); break; }
    case 266: { char abs[512]; at_resolve((int)a2,(const char*)a3,abs,sizeof(abs)); ret=vfs_create_symlink(abs,(const char*)a1) ? 0 : -(int64_t)EEXIST; break; }
    case 267: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); ret=(int64_t)fd_readlink(abs,(char*)a3,a4); break; }
    case 268: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); ret=(int64_t)vfs_chmod(abs,(uint32_t)a3); break; }
    case 269: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); ret=vfs_lookup(abs) ? 0 : -(int64_t)ENOENT; break; }
    case 270:
        ret = sys_pselect6((int) a1, (void*) a2, (void*) a3, (void*) a4, (void*) a5, (void*) a6);
        break;
    case 271:
        ret = sys_ppoll((struct pollfd_s*) a1, a2, (void*) a3, (const void*) a4, a5);
        break;
    case 272: ret = 0; break; /* unshare */
    case 273:
        ret = sys_set_robust_list((void*)a1, a2);
        break;
    case 274: case 275: case 276: case 277:
        ret = -(int64_t)ENOSYS; break; /* splice/tee/sync_file_range/vmsplice */
    case 278: ret = -(int64_t)ENOSYS; break; /* move_pages */
    case 279:
        ret = 0; /* utimensat */
        break;
    case 280: ret = fd_openat((int)a1, (const char*)a2, (int)a3, (int)a4); break; /* openat2 */
    case 281: { int _tmo = a5 ? (int)(((uint64_t*)a5)[0]*1000) : -1; ret = sys_epoll_wait((int)a1,(struct epoll_event*)a2,(int)a3,_tmo); break; } /* epoll_pwait */
    case 282: ret = -(int64_t)ENOSYS; break; /* accept4: no network */
    case 283: case 284: ret = -(int64_t)ENOSYS; break; /* signalfd4, eventfd2 */
    case 285: ret = 0; break; /* fallocate: no-op */
    case 286: ret = -(int64_t)ENOSYS; break; /* inotify_init1 */
    case 287: ret = fd_pipe((int*)a1); break; /* pipe2: ignore O_CLOEXEC/O_NONBLOCK */
    case 288: ret = -(int64_t)ENOSYS; break; /* inotify_init1 (duplicate) */
    case 289: ret = sys_preadv((int)a1, (const struct iovec*)a2, (int)a3, a4); break;
    case 290: ret = sys_pwritev((int)a1, (const struct iovec*)a2, (int)a3, a4); break;
    case 291: ret = sys_epoll_create1((int)a1); break; /* epoll_create1 */
    case 292:
        ret = fd_dup3((int)a1, (int)a2, (int)a3);
        break;
    case 293:
        ret = fd_pipe((int*)a1);
        break;
    case 295: case 296: ret = -(int64_t)ENOSYS; break; /* fanotify */
    case 300: ret = 0; break; /* clock_adjtime */
    case 301: ret = 0; break; /* syncfs: no-op */
    case 302:
        ret = sys_prlimit64(a1, a2, (void*) a3, (void*) a4);
        break;
    case 303: ret = 0; break; /* finit_module */
    case 304: ret = 0; break; /* sched_setattr */
    case 305: if (a2) memset((void*)a2, 0, 56); ret = 0; break; /* sched_getattr */
    case 306: { char ao[512], an[512]; at_resolve((int)a1,(const char*)a2,ao,sizeof(ao)); at_resolve((int)a3,(const char*)a4,an,sizeof(an)); ret=(int64_t)vfs_rename(ao,an); break; } /* renameat2 */
    case 307: ret = 0; break; /* seccomp: allow all */
    case 318:
        ret = sys_getrandom((void*) a1, a2, (uint32_t) a3);
        break;
    case 319: ret = sys_memfd_create((const char*)a1, (uint32_t)a2); break;
    case 325: ret = 0; break; /* mlock2 */
    case 326: ret = sys_copy_file_range((int)a1,(uint64_t*)a2,(int)a3,(uint64_t*)a4,a5,(uint32_t)a6); break;
    case 327: ret = sys_preadv((int)a1,(const struct iovec*)a2,(int)a3,a4); break; /* preadv2 */
    case 328: ret = sys_pwritev((int)a1,(const struct iovec*)a2,(int)a3,a4); break; /* pwritev2 */
    case 332: ret = sys_statx((int)a1,(const char*)a2,(int)a3,(uint32_t)a4,(struct statx*)a5); break;
    case 334: { for (int _fd=(int)a1; _fd<=(int)a2 && _fd<VFS_FD_MAX; _fd++) if (fd_valid(_fd)) fd_close(_fd); ret=0; break; } /* close_range */
    case 439: { char abs[512]; at_resolve((int)a1,(const char*)a2,abs,sizeof(abs)); ret=vfs_lookup(abs)?0:-(int64_t)ENOENT; break; } /* faccessat2 */

    default:
        log_debug("[syscall %lu  a1=%lx a2=%lx a3=%lx]", nr, a1, a2, a3);
        ret = -(int64_t) ENOSYS;
        break;
    }
    f->rax = (uint64_t) ret;

    signal_check(f);
}
