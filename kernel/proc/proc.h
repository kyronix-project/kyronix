#pragma once
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/signal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROC_UNUSED 0
#define PROC_RUNNING 1
#define PROC_READY 2
#define PROC_WAITING 3
#define PROC_ZOMBIE 4
#define PROC_DYING 5
#define PROC_STOPPED 6
#define PROC_EMBRYO 7
#define PROC_QUARANTINED 8

#define PROC_MAX 64
#define KSTACK_PAGES 16
#define KSTACK_SIZE (KSTACK_PAGES * 4096ULL)

typedef struct proc {
    int state;
    int pgid;
    uint32_t pid;
    uint32_t ppid;
    vmm_space_t *space;
    uint8_t *kstack;
    uint64_t kstack_top;
    uint64_t kstack_rsp;
    uint64_t user_rsp;
    int exit_code;
    int wait_for;
    uint32_t refcount;
    uint64_t brk;
    uint64_t brk_base;
    uint64_t mmap_bump;
    uint64_t fs_base;
    vfs_file_t **fds;
    uint32_t *fds_refcnt;
    uint64_t pending_sigs;
    uint64_t sig_mask;
    k_sigaction_t sig_actions[NSIG];
    char cwd[512];
    uint64_t wakeup_tick;
    uint64_t alarm_tick;
    char exe_path[512];
    uint32_t *cleartid_addr;
    uint8_t is_thread;
    uint32_t uid, gid;
    uint32_t euid, egid;
    uint32_t suid, sgid;
    uint32_t fsuid, fsgid;
    uint32_t umask;
    uint64_t kstack_guard;
    uint64_t itimer_value_ms;
    uint64_t itimer_interval_ms;
    uint64_t itimer_next_tick;
    void *blocked_pipe;
    int blocked_pipe_read;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint8_t on_sigstack;  /* currently executing a handler on the alt stack */
    uint64_t pages_alloc; /* pages allocated via brk/mmap */
    uint64_t pages_freed; /* pages freed via munmap/shrink brk */
    uint32_t jail_id;     /* 0 = host; appended at end so sched.S offsets stay fixed */
    uint8_t jail_exempt;  /* inherited; init=1, suppresses auto-isolation */
    uint8_t phantom_sandbox; /* network/VFS simulation is enabled for this process */
    uint8_t phantom_pending; /* trap observed; clone at the next safe syscall point */
    uint16_t phantom_score;
    uint8_t phantom_clone_count;
    uint64_t phantom_score_tick;
    uint64_t phantom_last_clone_tick;
    uint8_t phantom_quarantined;
    uint8_t phantom_quarantine_action;
    uint8_t phantom_fault_quarantine;
    uint8_t phantom_fault_job_pending;
    uint32_t phantom_sandbox_pid;

    uint32_t tracer_pid;          /* 0 = not traced */
    uint8_t ptrace_stopped;       /* currently in ptrace-stop, waiting for tracer */
    uint8_t ptrace_reported;      /* this stop was already handed back via wait4 */
    uint8_t ptrace_stop_sig;      /* signal reported to the tracer for this stop */
    uint8_t ptrace_syscall_trace; /* PTRACE_SYSCALL: stop at syscall enter/exit */
    uint8_t ptrace_in_syscall;    /* toggles enter/exit for PTRACE_SYSCALL */
    uint8_t ptrace_step;          /* one-shot: set TF before next resume (PTRACE_SINGLESTEP) */
    uint8_t ptrace_frame_kind;    /* 0=none, 1=syscall_frame_t*, 2=cpu_state_t* */
    void *ptrace_frame;           /* frame the tracee is stopped in, valid while stopped */
    uint64_t ptrace_orig_rax;     /* syscall nr as of entry; rax itself gets clobbered by the
                                   * return value before an exit-stop can report it */

    uint8_t stop_sig;      /* job-control (SIGTSTP/SIGSTOP) stop signal, valid while PROC_STOPPED */
    uint8_t stop_reported; /* this stop was already handed back via wait4(WUNTRACED) */
    uint8_t job_stopped;   /* loop condition for proc_job_stop(); state alone can't be used since
                            * sched_yield_blocking() unconditionally resets state to PROC_WAITING */
} proc_t;

extern proc_t g_proctable[PROC_MAX] __attribute__((aligned(16)));

extern spinlock_t g_proctable_lock;

extern volatile uint64_t g_ready_mask;
extern volatile uint64_t g_used_mask;
extern volatile uint64_t g_timer_mask;

static inline int proc_slot(proc_t *p) { return (int) (p - g_proctable); }

static inline void proc_set_ready(proc_t *p) {
    int bit = proc_slot(p);
    __atomic_fetch_or(&g_used_mask, 1ULL << bit, __ATOMIC_RELAXED);
    __atomic_fetch_or(&g_ready_mask, 1ULL << bit, __ATOMIC_RELEASE);
}

static inline void proc_publish_ready(proc_t *p) {
    __atomic_store_n(&p->state, PROC_READY, __ATOMIC_RELEASE);
    proc_set_ready(p);
}

static inline void proc_clear_ready(proc_t *p) {
    __atomic_fetch_and(&g_ready_mask, ~(1ULL << proc_slot(p)), __ATOMIC_RELAXED);
}

static inline void proc_set_used(proc_t *p) {
    __atomic_fetch_or(&g_used_mask, 1ULL << proc_slot(p), __ATOMIC_RELAXED);
}

static inline void proc_clear_used(proc_t *p) {
    __atomic_fetch_and(&g_used_mask, ~(1ULL << proc_slot(p)), __ATOMIC_RELAXED);
    __atomic_fetch_and(&g_ready_mask, ~(1ULL << proc_slot(p)), __ATOMIC_RELAXED);
    __atomic_fetch_and(&g_timer_mask, ~(1ULL << proc_slot(p)), __ATOMIC_RELAXED);
}

static inline void proc_set_timer(proc_t *p) {
    __atomic_fetch_or(&g_timer_mask, 1ULL << proc_slot(p), __ATOMIC_RELAXED);
}

static inline void proc_ref(proc_t *p) { __atomic_fetch_add(&p->refcount, 1, __ATOMIC_RELAXED); }

void proc_kstack_free(proc_t *p);

static inline void proc_unref(proc_t *p) {
    if (__atomic_fetch_sub(&p->refcount, 1, __ATOMIC_ACQ_REL) == 1) {
        proc_kstack_free(p);
        p->state = PROC_UNUSED;
        proc_clear_used(p);
    }
}

#include "arch/x86_64/percpu.h"
static inline proc_t **__g_current_proc_slot(void) {
    uint32_t cpu_id;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(cpu_id));
    return (proc_t **) &g_cpu_local[cpu_id].current;
}
#define g_current_proc (*__g_current_proc_slot())

void proc_init(void);
proc_t *proc_alloc(uint32_t ppid);
proc_t *proc_create_idle(uint32_t cpu_id, void (*entry)(void));
proc_t *proc_create_kernel(const char *name, void (*entry)(void));
void proc_kstack_free(proc_t *p);
void proc_defer_thread_reap(proc_t *p);
void proc_ptrace_stop(proc_t *p, int sig, int frame_kind, void *frame, uint64_t *rflags_slot);
void proc_job_stop(proc_t *p, int sig);
void proc_reap_pending(void);
proc_t *proc_find(uint32_t pid);
proc_t *proc_next_ready(proc_t *skip);
proc_t *sched_claim_next(proc_t *skip);
proc_t *proc_idle_until_ready(proc_t *skip);
void sched_switch(proc_t *next);
void sched_block_current(void);
void sched_yield_blocking(void);
extern void proc_resume_frame(void);
extern void proc_resume_interrupt_frame(void);
__attribute__((noreturn)) void proc_do_exit(int code);
