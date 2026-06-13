#include "poll.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22
#define EINTR 4

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLNVAL 0x0020

extern volatile uint64_t g_ticks;

static int poll_check(struct pollfd_s* fds, uint64_t nfds)
{
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0)
            continue;
        if (!fd_valid(fds[i].fd)) {
            fds[i].revents = POLLNVAL;
        } else {
            if ((fds[i].events & POLLIN) && fd_pollin(fds[i].fd))
                fds[i].revents |= POLLIN;
            if ((fds[i].events & POLLOUT) && fd_pollout(fds[i].fd))
                fds[i].revents |= POLLOUT;
        }
        if (fds[i].revents)
            ready++;
    }
    return ready;
}

int64_t sys_poll(struct pollfd_s* fds, uint64_t nfds, int timeout)
{
    if (!fds && nfds)
        return -(int64_t) EFAULT;
    if (nfds && !uptr_ok_w(fds, nfds * sizeof(*fds)))
        return -(int64_t) EFAULT;
    int ready = nfds ? poll_check(fds, nfds) : 0;
    if (ready > 0 || timeout == 0)
        return ready;
    proc_t* p = g_current_proc;
    uint64_t deadline = (timeout > 0) ? g_ticks + (uint64_t) timeout : (uint64_t) -1ULL;
    while (!ready) {
        if (g_ticks >= deadline)
            break;
        if (p && (p->pending_sigs & ~p->sig_mask))
            return -(int64_t) EINTR;
        p->wakeup_tick = g_ticks + 5;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
        p->wakeup_tick = 0;
        if (nfds)
            ready = poll_check(fds, nfds);
    }
    return (int64_t) ready;
}

int64_t sys_ppoll(struct pollfd_s* fds, uint64_t nfds, void* tmo, const void* sigmask,
                  uint64_t sigsetsize)
{
    (void) sigmask;
    (void) sigsetsize;
    int timeout = -1;
    if (tmo) {
        uint64_t ms = ((uint64_t*) tmo)[0] * 1000 + ((uint64_t*) tmo)[1] / 1000000;
        timeout = ms > 0x7fffffff ? 0x7fffffff : (int) ms;
    }
    return sys_poll(fds, nfds, timeout);
}

static inline bool fds_test(const uint8_t* set, int fd)
{
    return set && ((set[fd >> 3] >> (fd & 7)) & 1);
}

static inline void fds_set(uint8_t* set, int fd)
{
    if (set)
        set[fd >> 3] |= (uint8_t) (1 << (fd & 7));
}

static int64_t sys_select_common(int nfds, void* rfds, void* wfds, void* efds, void* timeout,
                                 bool timeout_is_user)
{
    if (nfds < 0 || nfds > VFS_FD_MAX)
        return -(int64_t) EINVAL;
    uint64_t set_bytes = ((uint64_t) nfds + 7) / 8;
    if (rfds && (!uptr_ok(rfds, set_bytes) || !uptr_ok_w(rfds, set_bytes)))
        return -(int64_t) EFAULT;
    if (wfds && (!uptr_ok(wfds, set_bytes) || !uptr_ok_w(wfds, set_bytes)))
        return -(int64_t) EFAULT;
    if (efds && (!uptr_ok(efds, set_bytes) || !uptr_ok_w(efds, set_bytes)))
        return -(int64_t) EFAULT;
    if (timeout && timeout_is_user && !uptr_ok(timeout, 16))
        return -(int64_t) EFAULT;
    uint64_t deadline = (uint64_t) -1ULL;
    if (timeout) {
        uint64_t ms = ((uint64_t*) timeout)[0] * 1000 + ((uint64_t*) timeout)[1] / 1000;
        deadline = g_ticks + ms;
    }
    uint8_t rout[128], wout[128];
    proc_t* p = g_current_proc;
    for (;;) {
        memset(rout, 0, sizeof(rout));
        memset(wout, 0, sizeof(wout));
        int ready = 0;
        for (int fd = 0; fd < nfds && fd < VFS_FD_MAX; fd++) {
            if (fds_test((uint8_t*) rfds, fd) && fd_pollin(fd)) {
                fds_set(rout, fd);
                ready++;
            }
            if (fds_test((uint8_t*) wfds, fd) && fd_pollout(fd)) {
                fds_set(wout, fd);
                ready++;
            }
        }
        if (ready > 0 || g_ticks >= deadline) {
            if (rfds)
                memcpy(rfds, rout, set_bytes);
            if (wfds)
                memcpy(wfds, wout, set_bytes);
            if (efds)
                memset(efds, 0, set_bytes);
            return (int64_t) ready;
        }
        if (p && (p->pending_sigs & ~p->sig_mask))
            return -(int64_t) EINTR;
        p->wakeup_tick = g_ticks + 5;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
        p->wakeup_tick = 0;
    }
}

int64_t sys_select(int nfds, void* rfds, void* wfds, void* efds, void* timeout)
{
    return sys_select_common(nfds, rfds, wfds, efds, timeout, true);
}

int64_t sys_pselect6(int nfds, void* rfds, void* wfds, void* efds, void* timeout, void* sigmask)
{
    (void) sigmask;
    uint64_t tv[2] = {0, 0};
    if (timeout) {
        if (!uptr_ok(timeout, 16))
            return -(int64_t) EFAULT;
        tv[0] = ((uint64_t*) timeout)[0];
        tv[1] = ((uint64_t*) timeout)[1] / 1000;
    }
    return sys_select_common(nfds, rfds, wfds, efds, timeout ? tv : NULL, false);
}
