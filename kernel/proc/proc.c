#include "proc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/syscall_setup.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/jail.h"
#include "proc/smp.h"
#include "syscall/syscall.h"

_Static_assert(__builtin_offsetof(proc_t, space) == 16, "sched.S PROC_SPACE");
_Static_assert(__builtin_offsetof(proc_t, kstack_top) == 32, "sched.S PROC_KSTACK_TOP");
_Static_assert(__builtin_offsetof(proc_t, kstack_rsp) == 40, "sched.S PROC_KSTACK_RSP");
_Static_assert(__builtin_offsetof(proc_t, user_rsp) == 48, "sched.S PROC_USER_RSP");
_Static_assert(__builtin_offsetof(proc_t, fs_base) == 96, "sched.S PROC_FS_BASE");
_Static_assert(__builtin_offsetof(proc_t, fpu_state) == 3328, "sched.S PROC_FPU_STATE");

#define KSTACK_VA_BASE 0xffff920000000000ULL
#define KSTACK_VA_STRIDE ((KSTACK_PAGES + 1) * PAGE_SIZE)

static uint64_t g_kstack_va_bump = KSTACK_VA_BASE;

proc_t g_proctable[PROC_MAX];

spinlock_t g_proctable_lock = { .lock = 0 };

volatile uint64_t g_ready_mask = 0;
volatile uint64_t g_used_mask = 0;
volatile uint64_t g_timer_mask = 0;

void proc_init(void) {
    memset(g_proctable, 0, sizeof(g_proctable));
    log_info("PROC: table initialised  (%d slots)", PROC_MAX);
}

proc_t *proc_alloc(uint32_t ppid) {
    spin_lock(&g_proctable_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proctable[i].state != PROC_UNUSED) continue;

        proc_t *p = &g_proctable[i];
        memset(p, 0, sizeof(*p));

        p->state = PROC_EMBRYO;
        proc_set_used(p);
        p->pid = (uint32_t) (i + 1);
        p->ppid = ppid;
        p->pgid = (uint32_t) (i + 1);
        p->wait_for = 0;
        p->refcount = 1;

        uint64_t guard_va = g_kstack_va_bump;
        g_kstack_va_bump += KSTACK_VA_STRIDE;
        for (int pg = 0; pg < KSTACK_PAGES; pg++) {
            void *phys = pmm_alloc_zeroed();
            if (!phys) {
                /* free already-mapped pages */
                for (int j = 0; j < pg; j++) {
                    uint64_t va = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
                    vmm_unmap(&g_kernel_space, va);
                    if (pa) pmm_free((void *) pa);
                }
                p->state = PROC_UNUSED;
                proc_clear_used(p);
                spin_unlock(&g_proctable_lock);
                return NULL;
            }
            uint64_t va = guard_va + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
            if (vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0) {
                pmm_free(phys);
                for (int j = 0; j < pg; j++) {
                    uint64_t jva = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, jva);
                    vmm_unmap(&g_kernel_space, jva);
                    if (pa) pmm_free((void *) pa);
                }
                p->state = PROC_UNUSED;
                proc_clear_used(p);
                spin_unlock(&g_proctable_lock);
                return NULL;
            }
        }
        p->kstack_guard = guard_va;
        p->kstack = (uint8_t *) (guard_va + PAGE_SIZE);
        p->kstack_top = guard_va + KSTACK_VA_STRIDE;

        p->fds = (vfs_file_t **) kcalloc(VFS_FD_MAX, sizeof(vfs_file_t *));
        if (!p->fds) {
            proc_kstack_free(p);
            p->state = PROC_UNUSED;
            proc_clear_used(p);
            spin_unlock(&g_proctable_lock);
            return NULL;
        }
        p->fds_refcnt = (uint32_t *) kmalloc(sizeof(uint32_t));
        if (!p->fds_refcnt) {
            kfree(p->fds);
            p->fds = NULL;
            proc_kstack_free(p);
            p->state = PROC_UNUSED;
            proc_clear_used(p);
            spin_unlock(&g_proctable_lock);
            return NULL;
        }
        *p->fds_refcnt = 1;

        p->mmap_bump = 0x0000500000000000ULL;
        p->umask = 0022;
        p->cwd[0] = '/';
        p->cwd[1] = '\0';

        /* default x87 fpu + sse state: clean area, mask all exceptions */
        memset(p->fpu_state, 0, sizeof(p->fpu_state));
        ((uint16_t *) p->fpu_state)[0] = 0x037F;        /* FCW */
        ((uint32_t *) (p->fpu_state + 24))[0] = 0x1F80; /* MXCSR */

        spin_unlock(&g_proctable_lock);
        return p;
    }
    spin_unlock(&g_proctable_lock);
    return NULL;
}

void proc_reap_pending(void) {
    proc_t *p = g_reap_thread;
    if (!p || p == g_current_proc) return; /* never free the stack we are running on */
    g_reap_thread = NULL;
    proc_unref(p);
}

void proc_defer_thread_reap(proc_t *p) {
    proc_reap_pending(); /* flush any previous one first (not on its stack) */
    g_reap_thread = p;
}

void proc_kstack_free(proc_t *p) {
    if (!p->kstack_guard) return;
    for (int pg = 0; pg < KSTACK_PAGES; pg++) {
        uint64_t va = p->kstack_guard + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
        uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
        vmm_unmap(&g_kernel_space, va);
        if (pa) pmm_free((void *) pa);
    }
    p->kstack_guard = 0;
    p->kstack = NULL;
}

proc_t *proc_find(uint32_t pid) {
    spin_lock(&g_proctable_lock);
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proctable[i].state != PROC_UNUSED &&
            g_proctable[i].state != PROC_EMBRYO &&
            g_proctable[i].pid == pid) {
            proc_ref(&g_proctable[i]);
            spin_unlock(&g_proctable_lock);
            return &g_proctable[i];
        }
    }
    spin_unlock(&g_proctable_lock);
    return NULL;
}

proc_t *proc_next_ready(proc_t *skip) {
    static uint32_t g_last_scheduled[MAX_CPUS] = { 0 };
    uint32_t cpu = this_cpu_id();
    uint64_t ready = __atomic_load_n(&g_ready_mask, __ATOMIC_ACQUIRE);
    int start = ((int) g_last_scheduled[cpu] + 1) % PROC_MAX;

    /* Fast path: bitmap scan from start, wrap around */
    uint64_t mask = ready >> start;
    while (mask) {
        int bit = __builtin_ctzll(mask) + start;
        if (&g_proctable[bit] != skip && g_proctable[bit].pid != 0) {
            g_last_scheduled[cpu] = (uint32_t) bit;
            return &g_proctable[bit];
        }
        mask &= mask - 1;
    }
    mask = ready & ((1ULL << start) - 1);
    while (mask) {
        int bit = __builtin_ctzll(mask);
        if (&g_proctable[bit] != skip && g_proctable[bit].pid != 0) {
            g_last_scheduled[cpu] = (uint32_t) bit;
            return &g_proctable[bit];
        }
        mask &= mask - 1;
    }

    return NULL;
}

proc_t *sched_claim_next(proc_t *skip) {
    proc_t *next = proc_next_ready(skip);
    if (!next) return NULL;
    if (__sync_bool_compare_and_swap(&next->state, PROC_READY, PROC_RUNNING)) {
        __atomic_fetch_and(&g_ready_mask, ~(1ULL << proc_slot(next)), __ATOMIC_RELAXED);
        return next;
    }
    return NULL;
}

proc_t *proc_idle_until_ready(proc_t *skip) {
    for (;;) {
        proc_t *nt = sched_claim_next(skip);
        if (nt) return nt;
        sti();
        hlt();
        cli();
    }
}

void sched_block_current(void) {
    proc_t *p = g_current_proc;
    proc_clear_ready(p);
    /*
     * A waker may have published us after the caller changed state but before
     * we reached the scheduler. Consume that wake locally instead of clearing
     * its ready bit and sleeping forever.
     */
    if (__sync_bool_compare_and_swap(&p->state, PROC_READY, PROC_RUNNING)) {
        proc_clear_ready(p);
        return;
    }

    for (;;) {
        proc_t *next = sched_claim_next(p);
        if (!next) {
            uint32_t cpu_id = this_cpu_id();
            next = (proc_t *) g_cpu_local[cpu_id].idle;
            if (!next || next->state == PROC_UNUSED) {
                next = NULL;
            } else {
                next->state = PROC_RUNNING;
            }
        }

        if (next) {
            vfs_set_fdtable(next->fds);
            g_current_space = next->space;
            cpu_set_kernel_stack(next->kstack_top);
            sched_switch(next);

            p->state = PROC_RUNNING;
            vfs_set_fdtable(p->fds);
            g_current_space = p->space;
            cpu_set_kernel_stack(p->kstack_top);
            return;
        }

        sti();
        cpu_relax();
        cli();

        if (p->state == PROC_READY) {
            proc_clear_ready(p);
            p->state = PROC_RUNNING;
            return;
        }
    }
}

void sched_yield_blocking(void) {
    g_current_proc->state = PROC_WAITING;
    sched_block_current();
}

proc_t *proc_create_idle(uint32_t cpu_id, void (*entry)(void)) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proctable[i].state != PROC_UNUSED) continue;
        proc_t *p = &g_proctable[i];
        memset(p, 0, sizeof(*p));

        p->state = PROC_READY;
        proc_set_ready(p);
        p->pid = 0;
        p->ppid = 0;
        p->space = &g_kernel_space;
        p->fds = NULL;

        uint64_t guard_va = g_kstack_va_bump;
        g_kstack_va_bump += KSTACK_VA_STRIDE;
        for (int pg = 0; pg < KSTACK_PAGES; pg++) {
            void *phys = pmm_alloc_zeroed();
            if (!phys) {
                for (int j = 0; j < pg; j++) {
                    uint64_t va = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    vmm_unmap(&g_kernel_space, va);
                }
                p->state = PROC_UNUSED;
                proc_clear_used(p);
                return NULL;
            }
            uint64_t va = guard_va + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
            if (vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0) {
                pmm_free(phys);
                for (int j = 0; j < pg; j++) {
                    uint64_t jva = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    vmm_unmap(&g_kernel_space, jva);
                }
                p->state = PROC_UNUSED;
                proc_clear_used(p);
                return NULL;
            }
        }
        p->kstack_guard = guard_va;
        p->kstack = (uint8_t *) (guard_va + PAGE_SIZE);
        p->kstack_top = guard_va + KSTACK_VA_STRIDE;

        memset(p->fpu_state, 0, sizeof(p->fpu_state));
        ((uint16_t *) p->fpu_state)[0] = 0x037F;
        ((uint32_t *) (p->fpu_state + 24))[0] = 0x1F80;

        uint8_t *ksp = p->kstack + KSTACK_SIZE;

        ksp -= 8;
        *(uint64_t *) ksp = (uint64_t) (uintptr_t) entry;

        ksp -= 6 * 8;
        memset(ksp, 0, 6 * 8);

        p->kstack_rsp = (uint64_t) ksp;

        return p;
    }
    return NULL;
}

proc_t *proc_create_kernel(const char *name, void (*entry)(void)) {
    if (!entry) return NULL;
    proc_t *p = proc_alloc(0);
    if (!p) return NULL;

    p->space = &g_kernel_space;
    p->is_thread = 1;
    p->jail_id = JAIL_HOST;
    p->jail_exempt = 1;
    if (name) strncpy(p->exe_path, name, sizeof(p->exe_path) - 1);

    uint8_t *ksp = p->kstack + KSTACK_SIZE;
    ksp -= 8;
    *(uint64_t *) ksp = (uint64_t) (uintptr_t) entry;
    ksp -= 6 * 8;
    memset(ksp, 0, 6 * 8);
    p->kstack_rsp = (uint64_t) ksp;

    proc_publish_ready(p);
    return p;
}

void proc_ptrace_stop(proc_t *p, int sig, int frame_kind, void *frame, uint64_t *rflags_slot) {
    p->ptrace_stop_sig = (uint8_t) sig;
    p->ptrace_frame_kind = (uint8_t) frame_kind;
    p->ptrace_frame = frame;
    p->ptrace_stopped = 1;
    p->state = PROC_WAITING;
    proc_clear_ready(p);

    proc_t *tracer = proc_find(p->tracer_pid);
    if (tracer && tracer->state != PROC_UNUSED) {
        proc_send_signal(tracer, SIGCHLD);
        if (tracer->state == PROC_WAITING) {
            tracer->state = PROC_READY;
            proc_set_ready(tracer);
        }
        proc_unref(tracer);
    }

    while (p->ptrace_stopped) { sched_yield_blocking(); }

    if (rflags_slot) {
        if (p->ptrace_step) {
            *rflags_slot |= 0x100ULL;
        } else {
            *rflags_slot &= ~0x100ULL;
        }
    }
    p->ptrace_step = 0;
    p->ptrace_frame_kind = 0;
    p->ptrace_frame = NULL;

    p->state = PROC_RUNNING;
    vfs_set_fdtable(p->fds);
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);
}

void proc_job_stop(proc_t *p, int sig) {
    p->stop_sig = (uint8_t) sig;
    p->stop_reported = 0;
    p->job_stopped = 1;
    p->state = PROC_STOPPED;
    proc_clear_ready(p);

    proc_t *parent = proc_find(p->ppid);
    if (parent && parent->state != PROC_UNUSED) {
        proc_send_signal(parent, SIGCHLD);
        if (parent->state == PROC_WAITING) {
            parent->state = PROC_READY;
            proc_set_ready(parent);
        }
        proc_unref(parent);
    }

    while (p->job_stopped) {
        sched_yield_blocking();
        if (p->job_stopped) {
            p->state = PROC_STOPPED;
            proc_clear_ready(p);
        }
    }

    p->state = PROC_RUNNING;
    vfs_set_fdtable(p->fds);
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);
}
