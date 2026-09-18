#include "futex.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/spinlock.h"
#include "internal.h"
#include "proc/proc.h"
#include "security/anti_toctou.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_FD 2
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_WAKE_OP 5
#define FUTEX_LOCK_PI 6
#define FUTEX_UNLOCK_PI 7
#define FUTEX_TRYLOCK_PI 8
#define FUTEX_WAIT_BITSET 9
#define FUTEX_WAKE_BITSET 10
#define FUTEX_WAIT_REQUEUE_PI 11
#define FUTEX_CMP_REQUEUE_PI 12
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_OP_SET 0
#define FUTEX_OP_ADD 1
#define FUTEX_OP_OR 2
#define FUTEX_OP_ANDN 3
#define FUTEX_OP_XOR 4

#define FUTEX_MAX_WAITERS PROC_MAX

typedef struct {
    uint32_t *uaddr;
    proc_t *proc;
    uint32_t bitset;
} futex_entry_t;

static futex_entry_t g_futex_tab[FUTEX_MAX_WAITERS];
static spinlock_t g_futex_lock;

void cleartid_wake(uint32_t *addr) {
    spin_lock(&g_futex_lock);
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
        if (g_futex_tab[i].uaddr == addr && g_futex_tab[i].proc) {
            proc_t *w = g_futex_tab[i].proc;
            if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                proc_set_ready(w);
            g_futex_tab[i].proc = NULL;
        }
    }
    spin_unlock(&g_futex_lock);
}

static uint32_t futex_atomic_op(uint32_t *uaddr, int op, uint32_t val) {
    uint32_t old;
    switch (op) {
    case FUTEX_OP_SET:
        old = *uaddr;
        *uaddr = val;
        return old;
    case FUTEX_OP_ADD:
        old = *uaddr;
        *uaddr = old + val;
        return old;
    case FUTEX_OP_OR:
        old = *uaddr;
        *uaddr = old | val;
        return old;
    case FUTEX_OP_ANDN:
        old = *uaddr;
        *uaddr = old & ~val;
        return old;
    case FUTEX_OP_XOR:
        old = *uaddr;
        *uaddr = old ^ val;
        return old;
    default:
        return *uaddr;
    }
}

static int futex_do_wait(uint32_t *uaddr, uint32_t val, void *timeout, uint32_t bitset) {
    if (*uaddr != val) return -(int)EAGAIN;
    proc_t *p = cur();
    if (!p) return -(int)EFAULT;
    spin_lock(&g_futex_lock);
    int slot = -1;
    for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
        if (!g_futex_tab[i].proc) { slot = i; break; }
    if (slot < 0) { spin_unlock(&g_futex_lock); return -(int)ENOMEM; }
    g_futex_tab[slot].uaddr = uaddr;
    g_futex_tab[slot].proc = p;
    g_futex_tab[slot].bitset = bitset;
    spin_unlock(&g_futex_lock);
    uint64_t deadline = 0;
    if (timeout) {
        if (!uptr_ok(timeout, 16)) {
            spin_lock(&g_futex_lock); g_futex_tab[slot].proc = NULL; spin_unlock(&g_futex_lock);
            return -(int)EFAULT;
        }
        uint64_t ms = ((uint64_t *)timeout)[0] * 1000 + ((uint64_t *)timeout)[1] / 1000000;
        if (ms) deadline = g_ticks + ms;
    }
    p->wakeup_tick = deadline;
    if (deadline) proc_set_timer(p);
    while (g_futex_tab[slot].proc == p) {
        if (deadline && g_ticks >= deadline) break;
        if (proc_next_ready(p)) sched_yield_blocking();
        else { sti(); hlt(); cli(); }
    }
    bool timed_out = deadline && g_ticks >= deadline;
    p->wakeup_tick = 0;
    spin_lock(&g_futex_lock);
    if (g_futex_tab[slot].proc == p) g_futex_tab[slot].proc = NULL;
    spin_unlock(&g_futex_lock);
    return timed_out ? -(int)ETIMEDOUT : 0;
}

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, void *timeout, uint32_t *uaddr2,
                  uint32_t val3) {
    if (!uaddr || !uptr_ok(uaddr, sizeof(*uaddr))) return -(int64_t)EFAULT;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    anti_toctou_observe_memory(
        (uint64_t) (uintptr_t) uaddr,
        cmd == FUTEX_WAIT ? ANTI_TOCTOU_MEM_WAIT : ANTI_TOCTOU_MEM_WAKE);
    switch (cmd) {
    case FUTEX_WAIT:
        return (int64_t)futex_do_wait(uaddr, val, timeout, 0xFFFFFFFF);
    case FUTEX_WAIT_BITSET:
        if (!val3) return -(int64_t)EINVAL;
        return (int64_t)futex_do_wait(uaddr, val, timeout, val3);
    case FUTEX_WAKE: {
        proc_t *self = cur();
        int woken = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
            if (g_futex_tab[i].uaddr == uaddr && g_futex_tab[i].proc &&
                (!self || g_futex_tab[i].proc->jail_id == self->jail_id)) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t)woken;
    }
    case FUTEX_WAKE_BITSET: {
        uint32_t bitset = val3;
        if (!bitset) return -(int64_t)EINVAL;
        proc_t *self = cur();
        int woken = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
            if (g_futex_tab[i].uaddr == uaddr && g_futex_tab[i].proc &&
                (g_futex_tab[i].bitset & bitset) &&
                (!self || g_futex_tab[i].proc->jail_id == self->jail_id)) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t)woken;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (!uaddr2 || !uptr_ok(uaddr2, sizeof(*uaddr2))) return -(int64_t)EFAULT;
        if (cmd == FUTEX_CMP_REQUEUE && *uaddr != val3) return -(int64_t)EAGAIN;
        uint32_t nr_wake = val;
        uint32_t nr_requeue = (uint32_t)(uint64_t)timeout;
        proc_t *self = cur();
        int woken = 0, requeued = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (g_futex_tab[i].uaddr != uaddr || !g_futex_tab[i].proc) continue;
            if (self && g_futex_tab[i].proc->jail_id != self->jail_id) continue;
            if ((uint32_t)woken < nr_wake) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            } else if ((uint32_t)requeued < nr_requeue) {
                g_futex_tab[i].uaddr = uaddr2;
                requeued++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t)(woken + requeued);
    }
    case FUTEX_WAKE_OP: {
        if (!uaddr2 || !uptr_ok(uaddr2, sizeof(*uaddr2))) return -(int64_t)EFAULT;
        int wake_op = (int)(uint64_t)timeout;
        int op_op = (wake_op >> 28) & 0xf;
        int op_cmp = (wake_op >> 24) & 0xf;
        uint32_t oparg = (uint32_t)((wake_op >> 12) & 0xfff);
        int cmparg = (wake_op & 0xfff);
        uint32_t old2 = futex_atomic_op(uaddr2, op_op, oparg);
        bool cond = false;
        switch (op_cmp) {
        case 0: cond = (old2 == (uint32_t)cmparg); break;
        case 1: cond = (old2 < (uint32_t)cmparg); break;
        case 2: cond = (old2 <= (uint32_t)cmparg); break;
        case 3: cond = (old2 != (uint32_t)cmparg); break;
        case 4: cond = (old2 > (uint32_t)cmparg); break;
        case 5: cond = (old2 >= (uint32_t)cmparg); break;
        default: break;
        }
        int woken = 0;
        if (val > 0) {
            proc_t *self = cur();
            spin_lock(&g_futex_lock);
            for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int)val; i++) {
                if (g_futex_tab[i].uaddr == uaddr && g_futex_tab[i].proc &&
                    (!self || g_futex_tab[i].proc->jail_id == self->jail_id)) {
                    proc_t *w = g_futex_tab[i].proc;
                    w->wakeup_tick = 0;
                    if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                        proc_set_ready(w);
                    g_futex_tab[i].proc = NULL;
                    woken++;
                }
            }
            spin_unlock(&g_futex_lock);
        }
        if (cond && val3 > 0) {
            proc_t *self = cur();
            spin_lock(&g_futex_lock);
            int w2 = 0;
            for (int i = 0; i < FUTEX_MAX_WAITERS && w2 < (int)val3; i++) {
                if (g_futex_tab[i].uaddr == uaddr2 && g_futex_tab[i].proc &&
                    (!self || g_futex_tab[i].proc->jail_id == self->jail_id)) {
                    proc_t *w = g_futex_tab[i].proc;
                    w->wakeup_tick = 0;
                    if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                        proc_set_ready(w);
                    g_futex_tab[i].proc = NULL;
                    w2++;
                }
            }
            spin_unlock(&g_futex_lock);
            woken += w2;
        }
        return (int64_t)woken;
    }
    case FUTEX_FD:
        if (val != 0) return -(int64_t)EINVAL;
        return -(int64_t)EINVAL; /* FUTEX_FD deprecated in Linux, return EINVAL */
    case FUTEX_LOCK_PI:
    case FUTEX_UNLOCK_PI:
    case FUTEX_TRYLOCK_PI:
    case FUTEX_WAIT_REQUEUE_PI:
    case FUTEX_CMP_REQUEUE_PI:
        return -(int64_t)ENOSYS; /* PI futexes: no RT scheduler to back them */
    default:
        return -(int64_t)ENOSYS;
    }
}
