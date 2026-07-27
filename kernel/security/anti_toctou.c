#include "anti_toctou.h"

#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/vmm.h"
#include "proc/proc.h"

#define ANTI_TOCTOU_DETECTOR_SLOTS 128u
#define ANTI_TOCTOU_DELAY_SLOTS 64u
#define ANTI_TOCTOU_WINDOW_US 250000u
#define ANTI_TOCTOU_COOLDOWN_US 50000u
#define ANTI_TOCTOU_ACCESS_THRESHOLD 12u
#define ANTI_TOCTOU_SWITCH_THRESHOLD 5u
#define ANTI_TOCTOU_KIND_VFS 1u
#define ANTI_TOCTOU_KIND_MEMORY 2u

typedef struct {
    uint64_t key;
    uint64_t window_start;
    uint64_t last_detection;
    uint64_t actors;
    uint32_t last_actor;
    uint16_t accesses;
    uint16_t switches;
    uint8_t operations;
    uint8_t kind;
} detector_entry_t;

typedef struct {
    proc_t *proc;
    uint64_t deadline;
    uint32_t delay_us;
} delay_entry_t;

static detector_entry_t g_detector[ANTI_TOCTOU_DETECTOR_SLOTS];
static delay_entry_t g_delays[ANTI_TOCTOU_DELAY_SLOTS];
static spinlock_t g_detector_lock;
static spinlock_t g_delay_lock;
static proc_t *g_delay_worker;
static volatile uint32_t g_enabled;
static uint64_t g_cycles_per_us = 1000u;
static uint64_t g_detection_sequence;

static volatile uint64_t g_vfs_observations;
static volatile uint64_t g_memory_observations;
static volatile uint64_t g_vfs_detections;
static volatile uint64_t g_memory_detections;
static volatile uint64_t g_delays_queued;
static volatile uint64_t g_delays_applied;
static volatile uint64_t g_queue_drops;
static volatile uint64_t g_total_delay_us;
static volatile uint32_t g_last_delay_us;

static inline uint64_t anti_toctou_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static uint64_t mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static uint64_t cycles_from_us(uint64_t us) {
    if (us > UINT64_MAX / g_cycles_per_us) return UINT64_MAX;
    return us * g_cycles_per_us;
}

static uint64_t detect_cycles_per_us(void) {
    uint32_t max_leaf, b, c, d;
    cpuid(0, &max_leaf, &b, &c, &d);
    if (max_leaf >= 0x15u) {
        uint32_t denominator, numerator, crystal_hz;
        cpuid(0x15u, &denominator, &numerator, &crystal_hz, &d);
        if (denominator && numerator && crystal_hz) {
            uint64_t hz = (uint64_t) crystal_hz * numerator / denominator;
            if (hz >= 1000000u) return hz / 1000000u;
        }
    }
    if (max_leaf >= 0x16u) {
        uint32_t base_mhz;
        cpuid(0x16u, &base_mhz, &b, &c, &d);
        if (base_mhz) return base_mhz;
    }
    return 1000u;
}

static void pending_delay_max(proc_t *p, uint32_t delay_us) {
    uint32_t old = __atomic_load_n(&p->anti_toctou_pending_us, __ATOMIC_RELAXED);
    while (old < delay_us &&
           !__atomic_compare_exchange_n(&p->anti_toctou_pending_us, &old,
                                        delay_us, false, __ATOMIC_RELEASE,
                                        __ATOMIC_RELAXED)) {
    }
}

static void observe(uint64_t resource, uint32_t operation, uint8_t kind) {
    if (!__atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE)) return;
    proc_t *p = g_current_proc;
    if (!p || !p->space || p->space == &g_kernel_space || !p->pid) return;

    if (kind == ANTI_TOCTOU_KIND_VFS)
        __atomic_fetch_add(&g_vfs_observations, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&g_memory_observations, 1, __ATOMIC_RELAXED);

    uint64_t key = mix64(resource ^ ((uint64_t) kind << 60));
    uint64_t now = anti_toctou_rdtsc();
    uint64_t window = cycles_from_us(ANTI_TOCTOU_WINDOW_US);
    uint64_t cooldown = cycles_from_us(ANTI_TOCTOU_COOLDOWN_US);
    uint32_t delay_us = 0;

    spin_lock(&g_detector_lock);
    detector_entry_t *e =
        &g_detector[key & (ANTI_TOCTOU_DETECTOR_SLOTS - 1u)];
    if (e->key != key || e->kind != kind ||
        now - e->window_start > window) {
        uint64_t last_detection =
            (e->key == key && e->kind == kind) ? e->last_detection : 0;
        memset(e, 0, sizeof(*e));
        e->key = key;
        e->kind = kind;
        e->window_start = now;
        e->last_detection = last_detection;
        e->last_actor = p->pid;
        e->actors = 1ULL << ((p->pid - 1u) & 63u);
        e->accesses = 1;
        e->operations = (uint8_t) operation;
    } else {
        if (e->last_actor != p->pid) {
            e->last_actor = p->pid;
            if (e->switches != UINT16_MAX) e->switches++;
        }
        e->actors |= 1ULL << ((p->pid - 1u) & 63u);
        if (e->accesses != UINT16_MAX) e->accesses++;
        e->operations |= (uint8_t) operation;

        bool multi_actor = (e->actors & (e->actors - 1u)) != 0;
        bool vfs_check_use =
            kind != ANTI_TOCTOU_KIND_VFS ||
            ((e->operations & ANTI_TOCTOU_VFS_CHECK) &&
             (e->operations &
              (ANTI_TOCTOU_VFS_USE | ANTI_TOCTOU_VFS_WRITE)));
        bool cooled =
            !e->last_detection || now - e->last_detection >= cooldown;
        if (multi_actor && vfs_check_use &&
            e->accesses >= ANTI_TOCTOU_ACCESS_THRESHOLD &&
            e->switches >= ANTI_TOCTOU_SWITCH_THRESHOLD && cooled) {
            uint64_t sequence = ++g_detection_sequence;
            uint64_t random = mix64(key ^ ((uint64_t) p->pid << 32) ^
                                    sequence);
            uint32_t span =
                ANTI_TOCTOU_MAX_DELAY_US - ANTI_TOCTOU_MIN_DELAY_US + 1u;
            delay_us =
                ANTI_TOCTOU_MIN_DELAY_US + (uint32_t) (random % span);
            e->last_detection = now;
            e->window_start = now;
            e->accesses = 0;
            e->switches = 0;
            e->actors = 0;
            e->operations = 0;
        }
    }
    spin_unlock(&g_detector_lock);

    if (!delay_us) return;
    if (kind == ANTI_TOCTOU_KIND_VFS)
        __atomic_fetch_add(&g_vfs_detections, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&g_memory_detections, 1, __ATOMIC_RELAXED);
    pending_delay_max(p, delay_us);
}

void anti_toctou_observe_vfs(uint64_t resource, uint32_t operation) {
    if (resource)
        observe(resource, operation & 0xffu, ANTI_TOCTOU_KIND_VFS);
}

void anti_toctou_observe_memory(uint64_t address, uint32_t operation) {
    proc_t *p = g_current_proc;
    if (!p || !p->space) return;
    uint64_t page = address & ~0xfffULL;
    uint64_t key = mix64(p->space->pml4_phys) ^ page;
    observe(key, operation & 0xffu, ANTI_TOCTOU_KIND_MEMORY);
}

static bool delay_enqueue(proc_t *p, uint32_t delay_us) {
    bool queued = false;
    spin_lock(&g_delay_lock);
    if (!g_delay_worker) {
        spin_unlock(&g_delay_lock);
        return false;
    }
    for (uint32_t i = 0; i < ANTI_TOCTOU_DELAY_SLOTS; i++) {
        if (g_delays[i].proc) continue;
        proc_ref(p);
        g_delays[i].proc = p;
        g_delays[i].delay_us = delay_us;
        g_delays[i].deadline =
            anti_toctou_rdtsc() + cycles_from_us(delay_us);
        p->state = PROC_WAITING;
        proc_clear_ready(p);
        if (g_delay_worker &&
            __sync_bool_compare_and_swap(&g_delay_worker->state,
                                         PROC_WAITING, PROC_READY))
            proc_set_ready(g_delay_worker);
        queued = true;
        break;
    }
    spin_unlock(&g_delay_lock);
    return queued;
}

void anti_toctou_safe_point(void) {
    proc_t *p = g_current_proc;
    if (!p || p->space == &g_kernel_space) return;
    if (!anti_toctou_enabled()) {
        __atomic_store_n(&p->anti_toctou_pending_us, 0,
                         __ATOMIC_RELEASE);
        return;
    }
    uint32_t delay_us =
        __atomic_exchange_n(&p->anti_toctou_pending_us, 0, __ATOMIC_ACQ_REL);
    if (!delay_us) return;
    if (!delay_enqueue(p, delay_us)) {
        __atomic_fetch_add(&g_queue_drops, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&g_delays_queued, 1, __ATOMIC_RELAXED);
    sched_block_current();
}

static bool next_delay(delay_entry_t *out, uint64_t *deadline) {
    int best = -1;
    uint64_t earliest = UINT64_MAX;
    spin_lock(&g_delay_lock);
    for (uint32_t i = 0; i < ANTI_TOCTOU_DELAY_SLOTS; i++) {
        if (g_delays[i].proc && g_delays[i].deadline < earliest) {
            earliest = g_delays[i].deadline;
            best = (int) i;
        }
    }
    if (best < 0) {
        g_delay_worker->state = PROC_WAITING;
        proc_clear_ready(g_delay_worker);
        spin_unlock(&g_delay_lock);
        return false;
    }
    *deadline = earliest;
    if ((int64_t) (anti_toctou_rdtsc() - earliest) < 0) {
        spin_unlock(&g_delay_lock);
        return true;
    }
    *out = g_delays[best];
    memset(&g_delays[best], 0, sizeof(g_delays[best]));
    *deadline = 0;
    spin_unlock(&g_delay_lock);
    return true;
}

static void __attribute__((noreturn)) anti_toctou_worker(void) {
    for (;;) {
        delay_entry_t job = { 0 };
        uint64_t deadline = 0;
        if (!next_delay(&job, &deadline)) {
            cli();
            sched_block_current();
            continue;
        }
        if (deadline) {
            sti();
            while ((int64_t) (anti_toctou_rdtsc() - deadline) < 0)
                cpu_relax();
            cli();
            continue;
        }

        proc_t *p = job.proc;
        if (__sync_bool_compare_and_swap(&p->state, PROC_WAITING,
                                         PROC_READY))
            proc_set_ready(p);
        __atomic_fetch_add(&g_delays_applied, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_total_delay_us, job.delay_us,
                           __ATOMIC_RELAXED);
        __atomic_store_n(&g_last_delay_us, job.delay_us, __ATOMIC_RELAXED);
        proc_unref(p);
    }
}

void anti_toctou_set_enabled(bool enabled) {
    __atomic_store_n(&g_enabled, enabled && g_delay_worker ? 1u : 0u,
                     __ATOMIC_RELEASE);
}

bool anti_toctou_enabled(void) {
    return __atomic_load_n(&g_enabled, __ATOMIC_ACQUIRE) != 0;
}

void anti_toctou_reset(void) {
    spin_lock(&g_detector_lock);
    memset(g_detector, 0, sizeof(g_detector));
    g_detection_sequence = 0;
    spin_unlock(&g_detector_lock);
    __atomic_store_n(&g_vfs_observations, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_memory_observations, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_vfs_detections, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_memory_detections, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_delays_queued, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_delays_applied, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_queue_drops, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_total_delay_us, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_delay_us, 0, __ATOMIC_RELAXED);
}

void anti_toctou_get_stats(anti_toctou_stats_t *out) {
    memset(out, 0, sizeof(*out));
    out->enabled = anti_toctou_enabled() ? 1u : 0u;
    out->min_delay_us = ANTI_TOCTOU_MIN_DELAY_US;
    out->max_delay_us = ANTI_TOCTOU_MAX_DELAY_US;
    out->queue_capacity = ANTI_TOCTOU_DELAY_SLOTS;
    out->vfs_observations =
        __atomic_load_n(&g_vfs_observations, __ATOMIC_RELAXED);
    out->memory_observations =
        __atomic_load_n(&g_memory_observations, __ATOMIC_RELAXED);
    out->vfs_detections =
        __atomic_load_n(&g_vfs_detections, __ATOMIC_RELAXED);
    out->memory_detections =
        __atomic_load_n(&g_memory_detections, __ATOMIC_RELAXED);
    out->delays_queued =
        __atomic_load_n(&g_delays_queued, __ATOMIC_RELAXED);
    out->delays_applied =
        __atomic_load_n(&g_delays_applied, __ATOMIC_RELAXED);
    out->queue_drops = __atomic_load_n(&g_queue_drops, __ATOMIC_RELAXED);
    out->total_delay_us =
        __atomic_load_n(&g_total_delay_us, __ATOMIC_RELAXED);
    out->last_delay_us =
        __atomic_load_n(&g_last_delay_us, __ATOMIC_RELAXED);
}

bool anti_toctou_init(void) {
    if (g_delay_worker) return true;
    g_cycles_per_us = detect_cycles_per_us();
    anti_toctou_reset();
    g_delay_worker =
        proc_create_kernel("[jitter-worker]", anti_toctou_worker);
    if (!g_delay_worker) {
        log_error("ANTI-TOCTOU: could not create jitter worker");
        anti_toctou_set_enabled(false);
        return false;
    }
    anti_toctou_set_enabled(true);
    log_info("ANTI-TOCTOU: worker pid=%u tsc=%lu cycles/us delay=%u..%u us",
             g_delay_worker->pid, g_cycles_per_us,
             ANTI_TOCTOU_MIN_DELAY_US, ANTI_TOCTOU_MAX_DELAY_US);
    return true;
}
