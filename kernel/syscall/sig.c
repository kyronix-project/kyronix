#include "sig.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/syscall_setup.h"
#include "internal.h"
#include "lib/string.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "proc/signal.h"

static bool kill_permitted(const proc_t *sender, const proc_t *target) {
    if (!sender) return true; /* kernel */
    if (sender->euid == 0) return true;
    return sender->uid == target->uid || sender->uid == target->euid ||
           sender->euid == target->uid || sender->euid == target->euid;
}

int64_t sys_kill(int64_t pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -(int64_t) EINVAL;

    proc_t *self = cur();

    // sig == 0 is an existence/permission probe: validate the target but deliver nothing
    if (pid > 0) {
        proc_t *target = proc_find((uint32_t) pid);
        if (!target) return -(int64_t) ESRCH;
        if (!jail_can_see(self, target)) {
            proc_unref(target);
            return -(int64_t) ESRCH;
        }
        if (!kill_permitted(self, target)) {
            proc_unref(target);
            return -(int64_t) EPERM;
        }
        if (sig) proc_send_signal(target, sig);
        proc_unref(target);
    } else if (pid == -1) {
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (g_proctable[i].pid == 1) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (sig) proc_send_signal(&g_proctable[i], sig);
        }
    } else if (pid < -1) {
        uint32_t pgid = (uint32_t) (-pid);
        bool found = false;
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (g_proctable[i].pgid == (int) pgid) {
                if (sig) proc_send_signal(&g_proctable[i], sig);
                found = true;
            }
        }
        if (!found) return -(int64_t) ESRCH;
    } else {
        for (int i = 0; i < PROC_MAX; i++) {
            if (g_proctable[i].state == PROC_UNUSED) continue;
            if (!jail_can_see(self, &g_proctable[i])) continue;
            if (self && g_proctable[i].pgid == self->pgid && sig)
                proc_send_signal(&g_proctable[i], sig);
        }
    }
    return 0;
}

int64_t sys_rt_sigaction(int sig, const k_sigaction_t *act, k_sigaction_t *oldact,
                         uint64_t sigsetsize) {
    (void) sigsetsize;
    if (sig < 1 || sig >= NSIG) return -(int64_t) EINVAL;
    if (sig == SIGKILL || sig == SIGSTOP) return -(int64_t) EINVAL;

    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    if (oldact && !uptr_ok_w(oldact, sizeof(k_sigaction_t))) return -(int64_t) EFAULT;
    if (act && !uptr_ok(act, sizeof(k_sigaction_t))) return -(int64_t) EFAULT;
    if (act && act->sa_handler != SIG_DFL && act->sa_handler != SIG_IGN &&
        act->sa_handler >= USER_LIMIT)
        return -(int64_t) EINVAL;
    if (oldact) memcpy(oldact, &p->sig_actions[sig - 1], sizeof(k_sigaction_t));
    if (act) memcpy(&p->sig_actions[sig - 1], act, sizeof(k_sigaction_t));
    return 0;
}

int64_t sys_rt_sigprocmask(int how, const uint64_t *set, uint64_t *oldset, uint64_t sigsetsize) {
    (void) sigsetsize;
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;

    if (oldset) {
        if (!uptr_ok_w(oldset, 8)) return -(int64_t) EFAULT;
        *oldset = p->sig_mask;
    }
    if (!set) return 0;
    if (!uptr_ok(set, 8)) return -(int64_t) EFAULT;

    uint64_t bits = *set & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    switch (how) {
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

int64_t sys_rt_sigreturn(syscall_frame_t *f) {
    rt_sigframe_t *frame = (rt_sigframe_t *) (cpu_get_user_rsp() - 8);
    if (!uptr_ok(frame, sizeof(*frame))) return -(int64_t) EFAULT;
    mcontext_t *mc = &frame->uc.uc_mcontext;

    if (mc->rip >= USER_LIMIT) return -(int64_t) EFAULT;

    f->r8 = mc->r8;
    f->r9 = mc->r9;
    f->r10 = mc->r10;
    f->r11 = user_rflags(mc->eflags); // user rflags
    f->r12 = mc->r12;
    f->r13 = mc->r13;
    f->r14 = mc->r14;
    f->r15 = mc->r15;
    f->rdi = mc->rdi;
    f->rsi = mc->rsi;
    f->rbp = mc->rbp;
    f->rbx = mc->rbx;
    f->rdx = mc->rdx;
    f->rcx = mc->rip; // user rip

    cpu_set_user_rsp(mc->rsp);

    proc_t *p = cur();
    p->sig_mask = frame->uc.uc_sigmask;
    p->sig_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    p->on_sigstack = 0; // left the alt stack
    return (int64_t) mc->rax;
}

int64_t sys_pause(void) {
    proc_t *p = cur();
    if (!p) return 0;
    while (!(p->pending_sigs & ~p->sig_mask)) {
        proc_t *next = NULL;
        for (int i = 0; i < PROC_MAX; i++) {
            if (&g_proctable[i] == p) continue;
            if (g_proctable[i].state == PROC_READY) {
                next = &g_proctable[i];
                break;
            }
        }
        if (!next) {
            cpu_set_kernel_stack(p->kstack_top);
            sti();
            hlt();
            cli();
            continue;
        }
        p->state = PROC_WAITING;
        proc_clear_ready(p);
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        // IF=0 across the switch: keep current-proc/space updates atomic
        sched_switch(next);
        p->state = PROC_RUNNING;
        vfs_set_fdtable(p->fds);
        g_current_space = p->space;
        cpu_set_kernel_stack(p->kstack_top);
    }
    return -(int64_t) EINTR;
}

int64_t sys_rt_sigsuspend(const uint64_t *mask, uint64_t sigsetsize) {
    (void) sigsetsize;
    proc_t *p = cur();
    if (!p) return 0;
    uint64_t saved = p->sig_mask;
    if (mask) {
        if (!uptr_ok(mask, 8)) return -(int64_t) EFAULT;
        p->sig_mask = *mask & ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    }
    while (!(p->pending_sigs & ~p->sig_mask)) {
        proc_t *next = NULL;
        for (int i = 0; i < PROC_MAX; i++) {
            if (&g_proctable[i] == p) continue;
            if (g_proctable[i].state == PROC_READY) {
                next = &g_proctable[i];
                break;
            }
        }
        if (!next) {
            cpu_set_kernel_stack(p->kstack_top);
            sti();
            hlt();
            cli();
            continue;
        }
        p->state = PROC_WAITING;
        proc_clear_ready(p);
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        // IF=0 across the switch: keep current-proc/space updates atomic
        sched_switch(next);
        p->state = PROC_RUNNING;
        vfs_set_fdtable(p->fds);
        g_current_space = p->space;
        cpu_set_kernel_stack(p->kstack_top);
    }
    p->sig_mask = saved;
    return -(int64_t) EINTR;
}

int64_t sys_sigaltstack(const void *ss, void *oss) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;

    // userspace stack_t: void* ss_sp @0, int ss_flags @8, size_t ss_size @16
    if (oss) {
        if (!uptr_ok_w(oss, 24)) return -(int64_t) EFAULT;
        uint8_t *o = (uint8_t *) oss;
        *(uint64_t *) (o + 0) = p->sig_altstack_sp;
        *(int *) (o + 8) = p->on_sigstack ? SS_ONSTACK : (p->sig_altstack_size ? 0 : SS_DISABLE);
        *(uint64_t *) (o + 16) = p->sig_altstack_size;
    }
    if (ss) {
        if (!uptr_ok(ss, 24)) return -(int64_t) EFAULT;
        if (p->on_sigstack) return -(int64_t) EPERM; // cannot change while running on it
        const uint8_t *s = (const uint8_t *) ss;
        int flags = *(const int *) (s + 8);
        if (flags & SS_DISABLE) {
            p->sig_altstack_sp = 0;
            p->sig_altstack_size = 0;
        } else {
            p->sig_altstack_sp = *(const uint64_t *) (s + 0);
            p->sig_altstack_size = *(const uint64_t *) (s + 16);
        }
    }
    return 0;
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    (void) tgid;
    return sys_kill((int64_t) tid, sig);
}

int64_t sys_rt_sigtimedwait(const uint64_t *set, void *info, const void *timeout,
                            uint64_t sigsetsize) {
    (void) sigsetsize;
    (void) info;
    proc_t *p = cur();
    if (!p || !set) return -(int64_t) EFAULT;
    if (!uptr_ok(set, 8)) return -(int64_t) EFAULT;
    uint64_t mask = *set;
    uint64_t deadline = (uint64_t) -1ULL;
    if (timeout) {
        if (!uptr_ok(timeout, 16)) return -(int64_t) EFAULT;
        uint64_t ms =
            ((const uint64_t *) timeout)[0] * 1000 + ((const uint64_t *) timeout)[1] / 1000000;
        if (ms) deadline = g_ticks + ms;
    }
    while (!(p->pending_sigs & mask)) {
        if (deadline != (uint64_t) -1ULL && g_ticks >= deadline) return -(int64_t) ETIMEDOUT;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
    }
    uint64_t bit = p->pending_sigs & mask;
    int sig = 1;
    while (!((bit >> (sig - 1)) & 1)) sig++;
    p->pending_sigs &= ~(1ULL << (sig - 1));
    return (int64_t) sig;
}
