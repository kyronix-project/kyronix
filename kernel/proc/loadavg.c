#include "loadavg.h"
#include "arch/x86_64/pit.h"
#include "lib/log.h"
#include "proc/proc.h"
#include "proc/smp.h"
#include <stdbool.h>
#include <stdint.h>

#define LOAD_FREQ (5 * PIT_HZ)

#define FIXED_1 (1 << LOAD_SHIFT)

#define EXP_1  1884
#define EXP_5  2014
#define EXP_15 2034

#define CALC_LOAD(load, exp, active)                                                              \
    do {                                                                                           \
        uint64_t __new = (load) * (exp) + (active) * (FIXED_1 - (exp));                            \
        uint64_t __r = __new + (FIXED_1 / 2);                                                      \
        (load) = (__r >> LOAD_SHIFT);                                                               \
    } while (0)

uint64_t avenrun[3] = { 0, 0, 0 };
volatile uint64_t nr_active = 0;

static int active_task_count(void) {
    int n = 0;
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = &g_proctable[i];
        /* idle processes (pid 0, one per CPU) must not count toward load,
         * matching how the scheduler excludes them via pid != 0 */
        if (__atomic_load_n(&p->pid, __ATOMIC_RELAXED) == 0)
            continue;
        int st = __atomic_load_n(&p->state, __ATOMIC_RELAXED);
        if (st == PROC_RUNNING || st == PROC_READY)
            n++;
    }
    return n;
}

void loadavg_init(void) {
    avenrun[0] = 0;
    avenrun[1] = 0;
    avenrun[2] = 0;
    nr_active = 0;
    log_info("LOADAVG: initialised");
}

void calc_load_tick(void) {
    static uint64_t ticks_since_sample = 0;
    static bool seeded = false;

    nr_active = (uint64_t) active_task_count();

    if (!seeded) {
        /* seed from the current load so a machine that boots busy (or fires
         * many workers immediately) converges quickly instead of starting
         * from an all-zero 1/5/15-min history */
        avenrun[0] = nr_active * FIXED_1;
        avenrun[1] = nr_active * FIXED_1;
        avenrun[2] = nr_active * FIXED_1;
        seeded = true;
        return;
    }

    ticks_since_sample++;
    if (ticks_since_sample < LOAD_FREQ)
        return;

    ticks_since_sample = 0;

    CALC_LOAD(avenrun[0], EXP_1, nr_active);
    CALC_LOAD(avenrun[1], EXP_5, nr_active);
    CALC_LOAD(avenrun[2], EXP_15, nr_active);
}
