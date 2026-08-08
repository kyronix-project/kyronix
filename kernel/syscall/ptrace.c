#include "ptrace.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "internal.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "proc/signal.h"

#define PTRACE_TRACEME 0
#define PTRACE_PEEKTEXT 1
#define PTRACE_PEEKDATA 2
#define PTRACE_POKETEXT 4
#define PTRACE_POKEDATA 5
#define PTRACE_CONT 7
#define PTRACE_KILL 8
#define PTRACE_SINGLESTEP 9
#define PTRACE_GETREGS 12
#define PTRACE_SETREGS 13
#define PTRACE_ATTACH 16
#define PTRACE_DETACH 17
#define PTRACE_SYSCALL 24
#define PTRACE_SETOPTIONS 0x4200

struct ptrace_user_regs {
    uint64_t r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8, rax, rcx, rdx, rsi, rdi;
    uint64_t orig_rax, rip, cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};

static void ptrace_fill_regs(proc_t *t, struct ptrace_user_regs *out) {
    memset(out, 0, sizeof(*out));
    out->cs = GDT_USER_CODE_SEL;
    out->ss = out->ds = out->es = out->fs = out->gs = GDT_USER_DATA_SEL;
    out->fs_base = t->fs_base;
    out->rsp = t->user_rsp;

    if (t->ptrace_frame_kind == 1) {
        syscall_frame_t *f = (syscall_frame_t *) t->ptrace_frame;
        out->r15 = f->r15;
        out->r14 = f->r14;
        out->r13 = f->r13;
        out->r12 = f->r12;
        out->rbp = f->rbp;
        out->rbx = f->rbx;
        out->r11 = f->r11;
        out->r10 = f->r10;
        out->r9 = f->r9;
        out->r8 = f->r8;
        out->rax = f->rax;
        out->rcx = f->rcx;
        out->rdx = f->rdx;
        out->rsi = f->rsi;
        out->rdi = f->rdi;
        out->orig_rax = t->ptrace_orig_rax;
        out->rip = f->rcx;
        out->eflags = f->r11;
    } else if (t->ptrace_frame_kind == 2) {
        cpu_state_t *s = (cpu_state_t *) t->ptrace_frame;
        out->r15 = s->r15;
        out->r14 = s->r14;
        out->r13 = s->r13;
        out->r12 = s->r12;
        out->rbp = s->rbp;
        out->rbx = s->rbx;
        out->r11 = s->r11;
        out->r10 = s->r10;
        out->r9 = s->r9;
        out->r8 = s->r8;
        out->rax = s->rax;
        out->rcx = s->rcx;
        out->rdx = s->rdx;
        out->rsi = s->rsi;
        out->rdi = s->rdi;
        out->orig_rax = t->ptrace_orig_rax;
        out->rip = s->rip;
        out->cs = s->cs;
        out->eflags = s->rflags;
        out->rsp = s->rsp;
        out->ss = s->ss;
    }
}

static int ptrace_store_regs(proc_t *t, const struct ptrace_user_regs *in) {
    if (in->rip >= USER_LIMIT || in->rsp >= USER_LIMIT) return -1;
    if (t->ptrace_frame_kind == 1) {
        syscall_frame_t *f = (syscall_frame_t *) t->ptrace_frame;
        f->r15 = in->r15;
        f->r14 = in->r14;
        f->r13 = in->r13;
        f->r12 = in->r12;
        f->rbp = in->rbp;
        f->rbx = in->rbx;
        f->r11 = user_rflags(in->eflags);
        f->r10 = in->r10;
        f->r9 = in->r9;
        f->r8 = in->r8;
        f->rax = in->rax;
        f->rcx = in->rip;
        f->rdx = in->rdx;
        f->rsi = in->rsi;
        f->rdi = in->rdi;
        t->user_rsp = in->rsp;
        return 0;
    }
    if (t->ptrace_frame_kind == 2) {
        cpu_state_t *s = (cpu_state_t *) t->ptrace_frame;
        s->r15 = in->r15;
        s->r14 = in->r14;
        s->r13 = in->r13;
        s->r12 = in->r12;
        s->rbp = in->rbp;
        s->rbx = in->rbx;
        s->r11 = in->r11;
        s->r10 = in->r10;
        s->r9 = in->r9;
        s->r8 = in->r8;
        s->rax = in->rax;
        s->rcx = in->rcx;
        s->rdx = in->rdx;
        s->rsi = in->rsi;
        s->rdi = in->rdi;
        s->rip = in->rip;
        s->rflags = user_rflags(in->eflags);
        s->rsp = in->rsp;
        return 0;
    }
    return -1;
}

static int64_t ptrace_rw_mem(proc_t *target, uint64_t addr, void *kbuf, uint64_t len, int write) {
    if (len == 0) return 0;
    if (addr >= USER_LIMIT || len > USER_LIMIT - addr) return -1;

    cli();
    vmm_space_t *saved_space = g_current_space;
    g_current_space = target->space;
    vmm_switch(target->space);

    int ok =
        write ? uptr_ok_w((void *) (uintptr_t) addr, len) : uptr_ok((void *) (uintptr_t) addr, len);
    if (ok) {
        if (write)
            memcpy((void *) (uintptr_t) addr, kbuf, len);
        else
            memcpy(kbuf, (void *) (uintptr_t) addr, len);
    }

    g_current_space = saved_space;
    vmm_switch(saved_space);
    sti();
    return ok ? 0 : -1;
}

static bool ptrace_may_attach(const proc_t *self, const proc_t *target) {
    if (!self) return true;
    if (self == target) return false;
    if (!jail_can_see(self, target)) return false;
    if (self->jail_id != target->jail_id && !jail_host_priv(self)) return false;
    if (jail_host_priv(self)) return true;
    if (target->uid != target->euid || target->uid != target->suid) return false;
    return self->uid == target->uid && self->euid == target->uid && self->suid == target->uid;
}

int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t addr, uint64_t data) {
    proc_t *self = cur();

    if (request == PTRACE_TRACEME) {
        proc_t *parent = proc_find(self->ppid);
        if (!parent) return -(int64_t) ESRCH;
        bool allowed = ptrace_may_attach(parent, self);
        if (allowed && !self->tracer_pid) self->tracer_pid = self->ppid;
        proc_unref(parent);
        return allowed ? 0 : -(int64_t) EPERM;
    }

    proc_t *t = proc_find((uint32_t) pid);
    if (!t || t->state == PROC_UNUSED) {
        if (t) proc_unref(t);
        return -(int64_t) ESRCH;
    }

    int64_t rc = 0;

    if (!jail_can_see(self, t)) {
        rc = -(int64_t) ESRCH;
        goto out;
    }

    if (request == PTRACE_ATTACH) {
        if (t->tracer_pid) {
            rc = -(int64_t) EPERM;
            goto out;
        }
        if (!ptrace_may_attach(self, t)) {
            rc = -(int64_t) EPERM;
            goto out;
        }
        t->tracer_pid = self->pid;
        proc_send_signal(t, SIGSTOP);
        goto out;
    }

    if (t->tracer_pid != self->pid) {
        rc = -(int64_t) ESRCH;
        goto out;
    }

    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA: {
        uint64_t val = 0;
        if (ptrace_rw_mem(t, addr, &val, sizeof(val), 0) < 0) {
            rc = -(int64_t) EIO;
            goto out;
        }
        if (!uptr_ok_w((void *) data, sizeof(val))) {
            rc = -(int64_t) EFAULT;
            goto out;
        }
        *(uint64_t *) data = val;
        break;
    }
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA: {
        uint64_t val = data;
        if (ptrace_rw_mem(t, addr, &val, sizeof(val), 1) < 0) {
            rc = -(int64_t) EIO;
            goto out;
        }
        break;
    }
    case PTRACE_GETREGS: {
        struct ptrace_user_regs regs;
        ptrace_fill_regs(t, &regs);
        if (!uptr_ok_w((void *) data, sizeof(regs))) {
            rc = -(int64_t) EFAULT;
            goto out;
        }
        memcpy((void *) data, &regs, sizeof(regs));
        break;
    }
    case PTRACE_SETREGS: {
        struct ptrace_user_regs regs;
        if (!uptr_ok((void *) data, sizeof(regs))) {
            rc = -(int64_t) EFAULT;
            goto out;
        }
        memcpy(&regs, (void *) data, sizeof(regs));
        if (ptrace_store_regs(t, &regs) < 0) {
            rc = -(int64_t) EIO;
            goto out;
        }
        break;
    }
    case PTRACE_CONT:
    case PTRACE_SYSCALL:
    case PTRACE_SINGLESTEP:
        if (!t->ptrace_stopped) {
            rc = -(int64_t) ESRCH;
            goto out;
        }
        if ((int) data > 0 && (int) data < NSIG)
            __atomic_fetch_or(&t->pending_sigs, (1ULL << ((int) data - 1)), __ATOMIC_RELAXED);
        t->ptrace_syscall_trace = (request == PTRACE_SYSCALL);
        t->ptrace_step = (request == PTRACE_SINGLESTEP);
        t->ptrace_stopped = 0;
        t->ptrace_reported = 0;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY)) proc_set_ready(t);
        break;
    case PTRACE_KILL:
        __atomic_fetch_or(&t->pending_sigs, (1ULL << (SIGKILL - 1)), __ATOMIC_RELAXED);
        t->ptrace_stopped = 0;
        t->ptrace_reported = 0;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY)) proc_set_ready(t);
        break;
    case PTRACE_DETACH:
        t->tracer_pid = 0;
        t->ptrace_syscall_trace = 0;
        if (t->ptrace_stopped) {
            t->ptrace_stopped = 0;
            t->ptrace_reported = 0;
            if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
                proc_set_ready(t);
        }
        break;
    case PTRACE_SETOPTIONS:
        break;
    default:
        rc = -(int64_t) EINVAL;
        break;
    }
out:
    proc_unref(t);
    return rc;
}
