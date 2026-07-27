#include "profiler.h"
#ifdef CONFIG_PROFILER

#include "lib/kallsyms.h"
#include "lib/printf.h"
#include "lib/string.h"

/* Freestanding qsort (insertion sort — fine for < 512 elements) */
static void _psort(void *base, size_t nmemb, size_t size, int (*cmp)(const void *, const void *)) {
    char *arr = (char *) base;
    for (size_t i = 1; i < nmemb; i++) {
        char tmp[128];
        memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0 && cmp(arr + (j - 1) * size, tmp) < 0) {
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        }
        memcpy(arr + j * size, tmp, size);
    }
}

#define KTEXT_LO 0xffffffff80000000ULL
#define KTEXT_HI 0xffffffff80040000ULL

static prof_ring_t g_ring;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t) hi << 32) | lo;
}

static inline uint64_t read_rip(void) {
    uint64_t rip;
    __asm__ volatile("lea (%%rip), %0" : "=r"(rip));
    return rip;
}

/* Walk the kernel stack for return addresses (same approach as kernel_backtrace) */
static int stack_walk(uint64_t rbp, uint64_t *out, int max) {
    int n = 0;
    uint64_t frame = rbp;
    /* Sanity: frame must be in kernel address space */
    if (frame < KTEXT_LO || frame >= KTEXT_HI) {
        /* Try scanning from current RBP upwards */
        __asm__ volatile("mov %%rbp, %0" : "=r"(frame));
    }
    while (frame >= KTEXT_LO && frame < KTEXT_HI && n < max) {
        uint64_t ret = *(volatile uint64_t *) (frame + 8);
        if (ret >= KTEXT_LO && ret < KTEXT_HI) { out[n++] = ret; }
        uint64_t next = *(volatile uint64_t *) frame;
        if (next <= frame || next >= KTEXT_HI) break;
        frame = next;
    }
    return n;
}

void prof_tick(uint64_t rip, uint32_t pid) {
    if (!g_ring.active) return;

    uint32_t idx = g_ring.write_idx;
    prof_sample_t *s = &g_ring.ring[idx];

    s->rip = rip;
    s->pid = pid;

    /* Walk kernel stack from current RBP */
    uint64_t rbp;
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    s->depth = (uint8_t) stack_walk(rbp, s->stack, PROF_STACK_DEPTH);

    /* Advance ring buffer */
    g_ring.write_idx = (idx + 1) & (PROF_RING_SIZE - 1);
    if (g_ring.count < PROF_RING_SIZE)
        g_ring.count++;
    else
        g_ring.dropped++;

    g_ring.total_ticks++;
}

void prof_start(void) {
    prof_reset();
    g_ring.active = 1;
}

void prof_stop(void) { g_ring.active = 0; }

void prof_reset(void) {
    g_ring.active = 0;
    g_ring.total_ticks = 0;
    g_ring.dropped = 0;
    g_ring.write_idx = 0;
    g_ring.count = 0;
    memset(g_ring.ring, 0, sizeof(g_ring.ring));
}

int prof_is_active(void) { return g_ring.active; }

typedef struct {
    uint64_t addr;
    uint64_t count;
} addr_count_t;

static int cmp_count_desc(const void *a, const void *b) {
    uint64_t ca = ((const addr_count_t *) a)->count;
    uint64_t cb = ((const addr_count_t *) b)->count;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

static int aggregate_top(addr_count_t *out, int max_out) {
    /* Collect unique addresses and counts from the ring buffer.
     * Use a simple open-addressed hash table on stack. */
    addr_count_t table[512];
    int table_n = 0;
    memset(table, 0, sizeof(table));

    uint32_t n = g_ring.count;
    uint32_t start = (g_ring.count < PROF_RING_SIZE) ? 0 : g_ring.write_idx;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) & (PROF_RING_SIZE - 1);
        uint64_t addr = g_ring.ring[idx].rip;

        /* Linear probe hash */
        uint32_t h = (uint32_t) (addr >> 3) & 511;
        while (table[h].addr != 0 && table[h].addr != addr) { h = (h + 1) & 511; }
        if (table[h].addr == 0) table[h].addr = addr;
        table[h].count++;
        if (table_n < 512) table_n++;
    }

    /* Compact non-zero entries */
    int out_n = 0;
    for (int i = 0; i < 512 && out_n < max_out; i++) {
        if (table[i].addr != 0) { out[out_n++] = table[i]; }
    }

    _psort(out, (size_t) out_n, sizeof(addr_count_t), cmp_count_desc);
    return out_n;
}

typedef struct {
    uint32_t pid;
    uint64_t count;
} pid_count_t;

static int cmp_pid_count(const void *a, const void *b) {
    uint64_t ca = ((const pid_count_t *) a)->count;
    uint64_t cb = ((const pid_count_t *) b)->count;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

static int aggregate_per_process(pid_count_t *out, int max_out) {
    pid_count_t table[64];
    memset(table, 0, sizeof(table));

    uint32_t n = g_ring.count;
    uint32_t start = (g_ring.count < PROF_RING_SIZE) ? 0 : g_ring.write_idx;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) & (PROF_RING_SIZE - 1);
        uint32_t pid = g_ring.ring[idx].pid;

        /* Find or insert */
        int slot = -1;
        for (int j = 0; j < 64; j++) {
            if (table[j].pid == pid) {
                slot = j;
                break;
            }
            if (table[j].pid == 0 && slot == -1) slot = j;
        }
        if (slot >= 0) {
            table[slot].pid = pid;
            table[slot].count++;
        }
    }

    int out_n = 0;
    for (int i = 0; i < 64 && out_n < max_out; i++) {
        if (table[i].pid != 0) out[out_n++] = table[i];
    }
    _psort(out, (size_t) out_n, sizeof(pid_count_t), cmp_pid_count);
    return out_n;
}

typedef struct {
    uint64_t callee;
    uint64_t caller;
    uint64_t count;
} call_edge_t;

static int cmp_edge_count(const void *a, const void *b) {
    uint64_t ca = ((const call_edge_t *) a)->count;
    uint64_t cb = ((const call_edge_t *) b)->count;
    if (cb > ca) return 1;
    if (cb < ca) return -1;
    return 0;
}

static int aggregate_callgraph(call_edge_t *out, int max_out) {
    call_edge_t table[256];
    memset(table, 0, sizeof(table));

    uint32_t n = g_ring.count;
    uint32_t start = (g_ring.count < PROF_RING_SIZE) ? 0 : g_ring.write_idx;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (start + i) & (PROF_RING_SIZE - 1);
        prof_sample_t *s = &g_ring.ring[idx];
        if (s->depth < 1) continue;

        uint64_t callee = s->rip;
        uint64_t caller = s->stack[0]; // first return address = caller

        int slot = -1;
        for (int j = 0; j < 256; j++) {
            if (table[j].callee == callee && table[j].caller == caller) {
                slot = j;
                break;
            }
            if (table[j].count == 0 && slot == -1) slot = j;
        }
        if (slot >= 0) {
            table[slot].callee = callee;
            table[slot].caller = caller;
            table[slot].count++;
        }
    }

    int out_n = 0;
    for (int i = 0; i < 256 && out_n < max_out; i++) {
        if (table[i].count > 0) out[out_n++] = table[i];
    }
    _psort(out, (size_t) out_n, sizeof(call_edge_t), cmp_edge_count);
    return out_n;
}

int prof_render(char *buf, uint64_t bufsz) {
    int pos = 0;
#define EMIT(...)                                                                                  \
    do {                                                                                           \
        int _n = snprintf(buf + pos, bufsz - (uint64_t) pos, __VA_ARGS__);                         \
        if (_n > 0) pos += _n;                                                                     \
        if ((uint64_t) pos >= bufsz) return (int) pos;                                             \
    } while (0)

    uint64_t ms = g_ring.total_ticks * 4; /* ~4ms per tick at 250 Hz */
    uint64_t total = g_ring.count;

    EMIT("=== Kyronix Kernel Profile ===\n");
    EMIT("Status:  %s\n", g_ring.active ? "RUNNING" : "STOPPED");
    EMIT("Samples: %lu  (buffer %u/%d)\n", total, g_ring.count, PROF_RING_SIZE);
    EMIT("Dropped: %lu\n", g_ring.dropped);
    EMIT("Uptime:  %lu.%03lu s  (ticks: %lu)\n", ms / 1000, ms % 1000, g_ring.total_ticks);
    EMIT("\n");

    /* Top hotspots */
    addr_count_t top[PROF_TOP_N];
    int top_n = aggregate_top(top, PROF_TOP_N);
    EMIT("--- Top %d hotspots ---\n", top_n < PROF_TOP_N ? top_n : PROF_TOP_N);
    EMIT(" #   Samples  %%     Address      Function\n");
    for (int i = 0; i < top_n && i < PROF_TOP_N; i++) {
        const char *name = kallsyms_lookup(top[i].addr);
        uint64_t pct = total ? (top[i].count * 100) / total : 0;
        EMIT("%2d  %7lu  %3lu%%  0x%016lx  %s\n", i + 1, top[i].count, pct, top[i].addr, name);
    }
    EMIT("\n");

    /* Call graph */
    call_edge_t edges[64];
    int edge_n = aggregate_callgraph(edges, 64);
    EMIT("--- Call graph (top %d) ---\n", edge_n < 64 ? edge_n : 64);
    EMIT("Callee                          Caller                          Count\n");
    for (int i = 0; i < edge_n && i < 64; i++) {
        const char *callee = kallsyms_lookup(edges[i].callee);
        const char *caller = kallsyms_lookup(edges[i].caller);
        EMIT("  %-30s <- %-30s %lu\n", callee, caller, edges[i].count);
    }
    EMIT("\n");

    /* Per-process */
    pid_count_t procs[64];
    int proc_n = aggregate_per_process(procs, 64);
    EMIT("--- Per-process ---\n");
    EMIT("  PID  Samples  %%\n");
    for (int i = 0; i < proc_n; i++) {
        uint64_t pct = total ? (procs[i].count * 100) / total : 0;
        EMIT("  %4u  %7lu  %3lu%%\n", procs[i].pid, procs[i].count, pct);
    }

#undef EMIT
    return pos;
}

void prof_print(void) {
    uint64_t ms = g_ring.total_ticks * 4;
    uint64_t total = g_ring.count;

    kprintf("\n=== Kyronix Kernel Profile ===\n");
    kprintf("Status:  %s\n", g_ring.active ? "RUNNING" : "STOPPED");
    kprintf("Samples: %lu  (buffer %u/%d)\n", total, g_ring.count, PROF_RING_SIZE);
    kprintf("Dropped: %lu\n", g_ring.dropped);
    kprintf("Uptime:  %lu.%03lu s  (ticks: %lu)\n\n", ms / 1000, ms % 1000, g_ring.total_ticks);

    /* Top hotspots */
    addr_count_t top[PROF_TOP_N];
    int top_n = aggregate_top(top, PROF_TOP_N);
    kprintf("--- Top %d hotspots ---\n", top_n < PROF_TOP_N ? top_n : PROF_TOP_N);
    kprintf(" #   Samples  %%     Address      Function\n");
    for (int i = 0; i < top_n && i < PROF_TOP_N; i++) {
        const char *name = kallsyms_lookup(top[i].addr);
        uint64_t pct = total ? (top[i].count * 100) / total : 0;
        kprintf("%2d  %7lu  %3lu%%  0x%016lx  %s\n", i + 1, top[i].count, pct, top[i].addr, name);
    }
    kprintf("\n");

    /* Call graph */
    call_edge_t edges[64];
    int edge_n = aggregate_callgraph(edges, 64);
    kprintf("--- Call graph (top %d) ---\n", edge_n < 64 ? edge_n : 64);
    for (int i = 0; i < edge_n && i < 64; i++) {
        const char *callee = kallsyms_lookup(edges[i].callee);
        const char *caller = kallsyms_lookup(edges[i].caller);
        kprintf("  %s <- %s  (%lu)\n", callee, caller, edges[i].count);
    }
    kprintf("\n");

    /* Per-process */
    pid_count_t procs[64];
    int proc_n = aggregate_per_process(procs, 64);
    kprintf("--- Per-process ---\n");
    kprintf("  PID  Samples  %%\n");
    for (int i = 0; i < proc_n; i++) {
        uint64_t pct = total ? (procs[i].count * 100) / total : 0;
        kprintf("  %4u  %7lu  %3lu%%\n", procs[i].pid, procs[i].count, pct);
    }
    kprintf("\n");
}

#endif
