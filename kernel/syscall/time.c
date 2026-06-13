#include "time.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/pit.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22

extern volatile uint64_t g_ticks;

int64_t sys_getrlimit(uint64_t r, void* rl)
{
    if (!rl)
        return -(int64_t) EINVAL;
    if (!uptr_ok_w(rl, 16))
        return -(int64_t) EFAULT;
    if (r == 7) {
        ((uint64_t*) rl)[0] = VFS_FD_MAX;
        ((uint64_t*) rl)[1] = VFS_FD_MAX;
        return 0;
    }
    ((uint64_t*) rl)[0] = 1024ULL * 1024ULL * 1024ULL;
    ((uint64_t*) rl)[1] = 1024ULL * 1024ULL * 1024ULL;
    return 0;
}

int64_t sys_prlimit64(uint64_t p, uint64_t r, void* nl, void* ol)
{
    (void) p;
    (void) nl;
    if (ol) {
        if (!uptr_ok_w(ol, 16))
            return -(int64_t) EFAULT;
        if (r == 7) {
            ((uint64_t*) ol)[0] = VFS_FD_MAX;
            ((uint64_t*) ol)[1] = VFS_FD_MAX;
        } else {
            ((uint64_t*) ol)[0] = 1024ULL * 1024ULL * 1024ULL;
            ((uint64_t*) ol)[1] = 1024ULL * 1024ULL * 1024ULL;
        }
    }
    return 0;
}

int64_t sys_nanosleep(void* r, void* m)
{
    (void) m;
    if (!r)
        return 0;
    if (!uptr_ok(r, 16))
        return -(int64_t) EFAULT;
    uint64_t sec = ((uint64_t*) r)[0];
    uint64_t nsec = ((uint64_t*) r)[1];
    uint64_t ms = sec * 1000 + nsec / 1000000;
    if (ms == 0)
        return 0;
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    uint64_t deadline = g_ticks + ms;
    p->wakeup_tick = deadline;
    while (g_ticks < deadline) {
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
    }
    p->wakeup_tick = 0;
    return 0;
}

int64_t sys_getitimer(int w, void* v)
{
    (void) w;
    proc_t* p = g_current_proc;
    if (!v || !p)
        return 0;
    if (!uptr_ok_w(v, 32))
        return -(int64_t) EFAULT;
    uint64_t* out = (uint64_t*) v;
    out[0] = p->itimer_interval_ms / 1000;
    out[1] = (p->itimer_interval_ms % 1000) * 1000;
    uint64_t rem = (p->itimer_next_tick > g_ticks) ? p->itimer_next_tick - g_ticks : 0;
    out[2] = rem / 1000;
    out[3] = (rem % 1000) * 1000;
    return 0;
}

int64_t sys_setitimer(int w, const void* n, void* o)
{
    (void) w;
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    if (o && !uptr_ok_w(o, 32))
        return -(int64_t) EFAULT;
    sys_getitimer(w, o);
    if (n) {
        if (!uptr_ok(n, 32))
            return -(int64_t) EFAULT;
        const uint64_t* nv = (const uint64_t*) n;
        p->itimer_interval_ms = nv[0] * 1000 + nv[1] / 1000;
        uint64_t val_ms = nv[2] * 1000 + nv[3] / 1000;
        p->itimer_next_tick = val_ms ? g_ticks + val_ms : 0;
        p->itimer_value_ms = val_ms;
    }
    return 0;
}

int64_t sys_clock_gettime(uint64_t c, void* t)
{
    (void) c;
    if (t) {
        if (!uptr_ok_w(t, 16))
            return -(int64_t) EFAULT;
        uint64_t ms = g_epoch_base * 1000 + g_ticks;
        ((uint64_t*) t)[0] = ms / 1000;
        ((uint64_t*) t)[1] = (ms % 1000) * 1000000ULL;
    }
    return 0;
}

int64_t sys_gettimeofday(void* tv, void* tz)
{
    (void) tz;
    if (tv) {
        if (!uptr_ok_w(tv, 16))
            return -(int64_t) EFAULT;
        uint64_t ms = g_epoch_base * 1000 + g_ticks;
        ((uint64_t*) tv)[0] = ms / 1000;
        ((uint64_t*) tv)[1] = (ms % 1000) * 1000ULL;
    }
    return 0;
}

int64_t sys_times(void* b)
{
    if (b) {
        if (!uptr_ok_w(b, 32))
            return -(int64_t) EFAULT;
        memset(b, 0, 32);
    }
    return 0;
}

int64_t sys_alarm(uint32_t seconds)
{
    proc_t* p = g_current_proc;
    if (!p)
        return 0;
    uint64_t prev = 0;
    if (p->alarm_tick && p->alarm_tick > g_ticks)
        prev = (p->alarm_tick - g_ticks + 999) / 1000;
    p->alarm_tick = seconds ? g_ticks + (uint64_t) seconds * 1000 : 0;
    return (int64_t) prev;
}

int64_t sys_clock_getres(uint64_t clk, void* res)
{
    (void) clk;
    if (res) {
        if (!uptr_ok_w(res, 16))
            return -(int64_t) EFAULT;
        ((uint64_t*) res)[0] = 0;
        ((uint64_t*) res)[1] = 1000000ULL;
    }
    return 0;
}

int64_t sys_sysinfo(struct sysinfo_s* si)
{
    if (!si)
        return -(int64_t) EFAULT;
    if (!uptr_ok_w(si, sizeof(*si)))
        return -(int64_t) EFAULT;
    memset(si, 0, sizeof(*si));
    si->uptime = (int64_t) (g_ticks / 1000);
    si->totalram = 256ULL * 1024 * 1024;
    si->freeram = 128ULL * 1024 * 1024;
    si->mem_unit = 1;
    si->procs = 1;
    return 0;
}
