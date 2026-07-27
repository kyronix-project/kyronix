#include "security/phantom.h"

#include "arch/x86_64/percpu.h"
#include "lib/log.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "proc/smp.h"
#include "syscall/procctl.h"

extern volatile uint64_t g_ticks;
static volatile uint32_t g_mode;
static volatile uint64_t g_sequence;
static volatile uint32_t g_write;
static phantom_event_t g_events[PHANTOM_EVENT_RING];

#define PHANTOM_FAULT_QUEUE_SIZE 8u

typedef struct {
    proc_t *source;
    cpu_state_t state;
    uint64_t address;
} phantom_fault_job_t;

static phantom_fault_job_t g_fault_jobs[PHANTOM_FAULT_QUEUE_SIZE];
static uint32_t g_fault_head;
static uint32_t g_fault_tail;
static uint32_t g_fault_count;
static spinlock_t g_fault_lock;
static proc_t *g_fault_worker;

void phantom_init(void) {
    memset(g_events, 0, sizeof(g_events));
#ifdef CONFIG_PHANTOM_FORKING
    g_mode = PHANTOM_AUDIT;
#else
    g_mode = PHANTOM_OFF;
#endif
    g_sequence = 0;
    g_write = 0;
    g_fault_head = 0;
    g_fault_tail = 0;
    g_fault_count = 0;
    g_fault_lock.lock = 0;
    g_fault_worker = NULL;
    log_info("PHANTOM: live trap hooks ready (disabled)");
}

void phantom_set_mode(uint32_t mode) {
    if (mode > PHANTOM_QUARANTINE) mode = PHANTOM_OFF;
    g_mode = mode;
    for (int i = 0; i < PROC_MAX; i++) {
        g_proctable[i].phantom_pending = 0;
        g_proctable[i].phantom_score = 0;
        g_proctable[i].phantom_score_tick = 0;
        g_proctable[i].phantom_last_clone_tick = 0;
        g_proctable[i].phantom_clone_count = 0;
    }
    log_info("PHANTOM: mode=%u", mode);
}

uint32_t phantom_get_mode(void) { return g_mode; }

uint32_t phantom_event_weight(uint32_t kind, uint32_t flags) {
    switch (kind) {
    case PHANTOM_EVENT_FAULT:
        return PHANTOM_SCORE_THRESHOLD;
    case PHANTOM_EVENT_ACCESS:
        if (flags & 1u) return 50u; /* execute denial */
        if (flags & 2u) return 30u; /* write denial */
        return 10u;                 /* lookup/read denial */
    case PHANTOM_EVENT_NETWORK:
        return 10u;
    case PHANTOM_EVENT_CRYPTO:
        return 80u;
    default:
        return 0u;
    }
}

static void phantom_score_event(uint32_t kind, uint32_t flags) {
    proc_t *p = g_current_proc;
    if (!p || p->phantom_sandbox || kind == PHANTOM_EVENT_CLONE) return;

    uint64_t now = g_ticks;
    if (p->phantom_score &&
        now - p->phantom_score_tick > PHANTOM_SCORE_WINDOW_TICKS)
        p->phantom_score = 0;
    p->phantom_score_tick = now;

    uint32_t score = (uint32_t) p->phantom_score + phantom_event_weight(kind, flags);
    p->phantom_score = (uint16_t) (score > UINT16_MAX ? UINT16_MAX : score);
    if (p->phantom_score < PHANTOM_SCORE_THRESHOLD) return;
    if (p->phantom_clone_count >= PHANTOM_CLONE_BUDGET) return;
    if (p->phantom_clone_count &&
        now - p->phantom_last_clone_tick < PHANTOM_CLONE_COOLDOWN_TICKS)
        return;

    p->phantom_pending = 1;
    p->phantom_score = 0;
}

static void phantom_record_owner(const proc_t *owner, uint32_t kind,
                                 uint32_t flags, uint64_t address,
                                 uint64_t instruction, const char *detail,
                                 bool score) {
    if (g_mode == PHANTOM_OFF) return;
    uint32_t slot = __atomic_fetch_add(&g_write, 1, __ATOMIC_RELAXED) % PHANTOM_EVENT_RING;
    phantom_event_t *event = &g_events[slot];
    event->sequence = __atomic_add_fetch(&g_sequence, 1, __ATOMIC_RELAXED);
    event->tick = g_ticks;
    event->pid = owner ? owner->pid : 0;
    event->jail_id = owner ? owner->jail_id : 0;
    event->kind = kind;
    event->flags = flags;
    event->address = address;
    event->instruction = instruction;
    if (detail) strncpy(event->detail, detail, PHANTOM_DETAIL_MAX - 1);
    else event->detail[0] = '\0';
    event->detail[PHANTOM_DETAIL_MAX - 1] = '\0';
    if (score && (g_mode == PHANTOM_TRAP || g_mode == PHANTOM_QUARANTINE))
        phantom_score_event(kind, flags);
}

void phantom_record(uint32_t kind, uint32_t flags, uint64_t address,
                    uint64_t instruction, const char *detail) {
    phantom_record_owner(g_current_proc, kind, flags, address, instruction,
                         detail, true);
}

void phantom_record_for(const struct proc *owner, uint32_t kind,
                        uint32_t flags, uint64_t address,
                        uint64_t instruction, const char *detail) {
    phantom_record_owner(owner, kind, flags, address, instruction, detail,
                         false);
}

bool phantom_fault_candidate(const cpu_state_t *state, uint64_t address) {
    if (g_mode == PHANTOM_OFF || !state || !g_current_proc) return false;
    bool present = (state->error_code & 1u) != 0;
    bool write = (state->error_code & 2u) != 0;
    bool execute = (state->error_code & 16u) != 0;
    if (!present || (!write && !execute)) return false;
    phantom_record(PHANTOM_EVENT_FAULT, (uint32_t) state->error_code, address,
                   state->rip, execute ? "user instruction violation" : "user write violation");
    return g_mode == PHANTOM_TRAP || g_mode == PHANTOM_QUARANTINE;
}

static bool phantom_proc_on_cpu(const proc_t *target) {
    uint32_t ncpu = g_cpu_count < MAX_CPUS ? g_cpu_count : MAX_CPUS;
    for (uint32_t i = 0; i < ncpu; i++)
        if (__atomic_load_n(&g_cpu_local[i].current, __ATOMIC_ACQUIRE) ==
            target)
            return true;
    return false;
}

static void phantom_wake_source(proc_t *source) {
    while (phantom_proc_on_cpu(source)) cpu_relax();
    spin_lock(&g_proctable_lock);
    if (source->state == PROC_QUARANTINED) {
        source->state = PROC_READY;
        proc_set_ready(source);
    }
    spin_unlock(&g_proctable_lock);
}

static bool phantom_fault_enqueue(proc_t *source, const cpu_state_t *state,
                                  uint64_t address) {
    spin_lock(&g_fault_lock);
    if (!g_fault_worker || g_fault_count == PHANTOM_FAULT_QUEUE_SIZE) {
        spin_unlock(&g_fault_lock);
        return false;
    }

    proc_ref(source);
    phantom_fault_job_t *job = &g_fault_jobs[g_fault_tail];
    job->source = source;
    job->state = *state;
    job->address = address;
    g_fault_tail = (g_fault_tail + 1u) % PHANTOM_FAULT_QUEUE_SIZE;
    g_fault_count++;

    if (__sync_bool_compare_and_swap(&g_fault_worker->state, PROC_WAITING,
                                     PROC_READY))
        proc_set_ready(g_fault_worker);
    spin_unlock(&g_fault_lock);
    return true;
}

static bool phantom_fault_dequeue(phantom_fault_job_t *job) {
    spin_lock(&g_fault_lock);
    if (!g_fault_count) {
        g_fault_worker->state = PROC_WAITING;
        proc_clear_ready(g_fault_worker);
        spin_unlock(&g_fault_lock);
        return false;
    }

    *job = g_fault_jobs[g_fault_head];
    memset(&g_fault_jobs[g_fault_head], 0,
           sizeof(g_fault_jobs[g_fault_head]));
    g_fault_head = (g_fault_head + 1u) % PHANTOM_FAULT_QUEUE_SIZE;
    g_fault_count--;
    spin_unlock(&g_fault_lock);
    return true;
}

static void phantom_fault_complete(proc_t *source, int64_t sandbox_pid) {
    bool terminate;
    spin_lock(&g_proctable_lock);
    source->phantom_fault_job_pending = 0;
    if (sandbox_pid > 0)
        source->phantom_sandbox_pid = (uint32_t) sandbox_pid;
    else
        source->phantom_quarantine_action = PHANTOM_CTL_TERMINATE;
    terminate =
        source->phantom_quarantine_action == PHANTOM_CTL_TERMINATE;
    if (terminate) source->phantom_quarantined = 0;
    spin_unlock(&g_proctable_lock);

    if (sandbox_pid > 0) {
        phantom_record_for(source, PHANTOM_EVENT_QUARANTINE,
                           PHANTOM_QUARANTINEF_READY,
                           (uint64_t) (uint32_t) sandbox_pid, source->pid,
                           "fault sandbox ready");
    } else {
        phantom_record_for(source, PHANTOM_EVENT_QUARANTINE,
                           PHANTOM_QUARANTINEF_TERMINATE, 0, source->pid,
                           "fault sandbox failed closed");
    }
    if (terminate) phantom_wake_source(source);
}

static void __attribute__((noreturn)) phantom_fault_worker(void) {
    for (;;) {
        phantom_fault_job_t job;
        if (!phantom_fault_dequeue(&job)) {
            sched_block_current();
            continue;
        }

        int64_t child =
            proc_phantom_fault_clone(job.source, &job.state, job.address);
        phantom_fault_complete(job.source, child);
        proc_unref(job.source);
    }
}

bool phantom_worker_start(void) {
    if (g_fault_worker) return true;
    g_fault_worker =
        proc_create_kernel("[phantom-worker]", phantom_fault_worker);
    if (!g_fault_worker) {
        log_error("PHANTOM: could not create fault worker");
        return false;
    }
    log_info("PHANTOM: fault worker pid=%u queue=%u", g_fault_worker->pid,
             PHANTOM_FAULT_QUEUE_SIZE);
    return true;
}

bool phantom_handle_fault(const cpu_state_t *state, uint64_t address) {
    proc_t *p = g_current_proc;
    if (g_mode != PHANTOM_QUARANTINE || !p || p->phantom_sandbox)
        return false;

    spin_lock(&g_proctable_lock);
    p->phantom_pending = 0;
    p->phantom_sandbox_pid = 0;
    p->phantom_quarantine_action = PHANTOM_CTL_STATUS;
    p->phantom_quarantined = 1;
    p->phantom_fault_quarantine = 1;
    p->phantom_fault_job_pending = 1;
    p->state = PROC_QUARANTINED;
    proc_clear_ready(p);
    spin_unlock(&g_proctable_lock);

    if (!phantom_fault_enqueue(p, state, address)) {
        spin_lock(&g_proctable_lock);
        p->phantom_quarantined = 0;
        p->phantom_fault_quarantine = 0;
        p->phantom_fault_job_pending = 0;
        p->state = PROC_RUNNING;
        spin_unlock(&g_proctable_lock);
        phantom_record(PHANTOM_EVENT_CLONE,
                       PHANTOM_CLONEF_FAILED | PHANTOM_CLONEF_WORKER,
                       address, state->rip, "fault worker queue full");
        return false;
    }

    p->phantom_last_clone_tick = g_ticks;
    if (p->phantom_clone_count < UINT8_MAX) p->phantom_clone_count++;
    phantom_record(PHANTOM_EVENT_QUARANTINE, PHANTOM_QUARANTINEF_HOLD,
                   address, state->rip, "fault source queued");

    sched_block_current();
    proc_do_exit(-11);
}

bool phantom_pending(void) {
    return g_current_proc && g_current_proc->phantom_pending != 0;
}

void phantom_safe_point(syscall_frame_t *frame) {
    if (!phantom_pending() || !g_current_proc || !g_current_proc->phantom_pending) return;
    g_current_proc->phantom_pending = 0;
    uint32_t mode = g_mode;
    if (mode == PHANTOM_TRAP || mode == PHANTOM_QUARANTINE) {
        int64_t child = proc_phantom_clone(frame);
        if (child < 0) {
            log_warn("PHANTOM: deferred clone failed pid=%u rc=%ld", g_current_proc->pid, child);
        } else {
            g_current_proc->phantom_last_clone_tick = g_ticks;
            if (g_current_proc->phantom_clone_count < UINT8_MAX)
                g_current_proc->phantom_clone_count++;
            if (mode == PHANTOM_QUARANTINE)
                phantom_quarantine_current((uint32_t) child);
        }
    }
}

void phantom_quarantine_current(uint32_t sandbox_pid) {
    proc_t *p = g_current_proc;
    if (!p || p->phantom_sandbox || !sandbox_pid) return;

    spin_lock(&g_proctable_lock);
    p->phantom_sandbox_pid = sandbox_pid;
    p->phantom_quarantine_action = PHANTOM_CTL_STATUS;
    p->phantom_quarantined = 1;
    phantom_record(PHANTOM_EVENT_QUARANTINE, PHANTOM_QUARANTINEF_HOLD,
                   sandbox_pid, p->pid, "source timeline quarantined");
    p->state = PROC_QUARANTINED;
    proc_clear_ready(p);
    spin_unlock(&g_proctable_lock);

    sched_block_current();

    uint8_t action =
        __atomic_load_n(&p->phantom_quarantine_action, __ATOMIC_ACQUIRE);
    p->phantom_quarantine_action = PHANTOM_CTL_STATUS;
    if (action == PHANTOM_CTL_TERMINATE) {
        int status = p->phantom_fault_quarantine ? -11 : -9;
        p->phantom_fault_quarantine = 0;
        proc_do_exit(status);
    }
    p->phantom_fault_quarantine = 0;
}

uint32_t phantom_read(phantom_event_t *out, uint32_t max_events) {
    if (!out || !max_events) return 0;
    uint32_t n = max_events < PHANTOM_EVENT_RING ? max_events : PHANTOM_EVENT_RING;
    uint32_t end = __atomic_load_n(&g_write, __ATOMIC_ACQUIRE);
    uint32_t available = end < PHANTOM_EVENT_RING ? end : PHANTOM_EVENT_RING;
    if (n > available) n = available;
    for (uint32_t i = 0; i < n; i++) out[i] = g_events[(end - n + i) % PHANTOM_EVENT_RING];
    return n;
}
