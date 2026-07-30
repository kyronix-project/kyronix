#include "futex.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/spinlock.h"
#include "internal.h"
#include "proc/proc.h"
#include "security/anti_toctou.h"

#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_REQUEUE 3
#define FUTEX_CMP_REQUEUE 4
#define FUTEX_PRIVATE_FLAG 128
#define FUTEX_CLOCK_REALTIME 256

#define FUTEX_MAX_WAITERS PROC_MAX

typedef struct {
    uint32_t *uaddr;
    proc_t *proc;
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

int64_t sys_futex(uint32_t *uaddr, int op, uint32_t val, void *timeout, uint32_t *uaddr2,
                  uint32_t val3) {
    if (!uaddr || !uptr_ok(uaddr, sizeof(*uaddr))) return -(int64_t) EFAULT;
    int cmd = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);
    anti_toctou_observe_memory(
        (uint64_t) (uintptr_t) uaddr,
        cmd == FUTEX_WAIT ? ANTI_TOCTOU_MEM_WAIT : ANTI_TOCTOU_MEM_WAKE);
    switch (cmd) {
    case FUTEX_WAIT: {
        if (*uaddr != val) return -(int64_t) EAGAIN;
        proc_t *p = cur();
        if (!p) return -(int64_t) EFAULT;
        spin_lock(&g_futex_lock);
        int slot = -1;
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++)
            if (!g_futex_tab[i].proc) {
                slot = i;
                break;
            }
        if (slot < 0) {
            spin_unlock(&g_futex_lock);
            return -(int64_t) ENOMEM;
        }
        g_futex_tab[slot].uaddr = uaddr;
        g_futex_tab[slot].proc = p;
        spin_unlock(&g_futex_lock);
        uint64_t deadline = 0;
        if (timeout) {
            if (!uptr_ok(timeout, 16)) {
                spin_lock(&g_futex_lock);
                g_futex_tab[slot].proc = NULL;
                spin_unlock(&g_futex_lock);
                return -(int64_t) EFAULT;
            }
            uint64_t ms = ((uint64_t *) timeout)[0] * 1000 + ((uint64_t *) timeout)[1] / 1000000;
            if (ms) deadline = g_ticks + ms;
        }
        p->wakeup_tick = deadline;
        if (deadline) proc_set_timer(p);
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
        spin_lock(&g_futex_lock);
        if (g_futex_tab[slot].proc == p) g_futex_tab[slot].proc = NULL;
        spin_unlock(&g_futex_lock);
        return timed_out ? -(int64_t) ETIMEDOUT : 0;
    }
    case FUTEX_WAKE: {
        proc_t *self = cur();
        int woken = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS && woken < (int) val; i++) {
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
        return (int64_t) woken;
    }
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE: {
        if (!uaddr2 || !uptr_ok(uaddr2, sizeof(*uaddr2))) return -(int64_t) EFAULT;
        if (cmd == FUTEX_CMP_REQUEUE && *uaddr != val3) return -(int64_t) EAGAIN;
        uint32_t nr_wake = val;
        uint32_t nr_requeue = (uint32_t) (uint64_t) timeout; // val2 rides in the timeout slot
        proc_t *self = cur();
        int woken = 0, requeued = 0;
        spin_lock(&g_futex_lock);
        for (int i = 0; i < FUTEX_MAX_WAITERS; i++) {
            if (g_futex_tab[i].uaddr != uaddr || !g_futex_tab[i].proc) continue;
            if (self && g_futex_tab[i].proc->jail_id != self->jail_id) continue;
            if ((uint32_t) woken < nr_wake) {
                proc_t *w = g_futex_tab[i].proc;
                w->wakeup_tick = 0;
                if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY))
                    proc_set_ready(w);
                g_futex_tab[i].proc = NULL;
                woken++;
            } else if ((uint32_t) requeued < nr_requeue) {
                g_futex_tab[i].uaddr = uaddr2; // move waiter to the new futex
                requeued++;
            }
        }
        spin_unlock(&g_futex_lock);
        return (int64_t) (woken + requeued);
    }
    default:
        return -(int64_t) ENOSYS;
    }
}
