#include "poll.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22
#define EINTR 4

#define POLLIN 0x0001
#define POLLOUT 0x0004
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

extern volatile uint64_t g_ticks;

static spinlock_t g_poll_lock = SPINLOCK_INIT;
static uint64_t g_poll_waiters;
#define POLL_WATCH_MAX 64
static void *g_poll_objects[PROC_MAX][POLL_WATCH_MAX];
static uint8_t g_poll_object_count[PROC_MAX];
static uint8_t g_poll_wildcard[PROC_MAX];

void poll_notify_object(const void *object) {
    uint64_t flags = irq_save();
    spin_lock(&g_poll_lock);
    uint64_t waiters = g_poll_waiters;
    g_poll_waiters = 0;
    while (waiters) {
        int slot = __builtin_ctzll(waiters);
        bool match = !object || g_poll_wildcard[slot];
        for (uint8_t i = 0; !match && i < g_poll_object_count[slot]; i++)
            match = g_poll_objects[slot][i] == object;
        if (!match) {
            waiters &= waiters - 1;
            continue;
        }
        proc_t *p = &g_proctable[slot];
        if (__sync_bool_compare_and_swap(&p->state, PROC_WAITING, PROC_READY))
            proc_set_ready(p);
        g_poll_waiters &= ~(1ULL << slot);
        g_poll_object_count[slot] = 0;
        g_poll_wildcard[slot] = 0;
        waiters &= waiters - 1;
    }
    spin_unlock(&g_poll_lock);
    irq_restore(flags);
}

void poll_notify(void) { poll_notify_object(NULL); }

bool poll_wait_once(uint64_t deadline, poll_ready_fn ready, void *ctx,
                    void *const *objects, uint32_t object_count, bool wildcard) {
    proc_t *p = g_current_proc;
    if (!p) return false;

    uint64_t flags = irq_save();
    spin_lock(&g_poll_lock);
    if ((deadline != UINT64_MAX && g_ticks >= deadline) ||
        (p->pending_sigs & ~p->sig_mask) || (ready && ready(ctx))) {
        spin_unlock(&g_poll_lock);
        irq_restore(flags);
        return false;
    }

    if (deadline != UINT64_MAX) {
        p->wakeup_tick = deadline;
        proc_set_timer(p);
    }
    p->state = PROC_WAITING;
    int slot = proc_slot(p);
    if (object_count > POLL_WATCH_MAX) {
        object_count = 0;
        wildcard = true;
    }
    for (uint32_t i = 0; i < object_count; i++) g_poll_objects[slot][i] = objects[i];
    g_poll_object_count[slot] = (uint8_t) object_count;
    g_poll_wildcard[slot] = wildcard || object_count == 0;
    g_poll_waiters |= 1ULL << slot;
    spin_unlock(&g_poll_lock);
    irq_restore(flags);

    sched_block_current();

    flags = irq_save();
    spin_lock(&g_poll_lock);
    slot = proc_slot(p);
    g_poll_waiters &= ~(1ULL << slot);
    g_poll_object_count[slot] = 0;
    g_poll_wildcard[slot] = 0;
    spin_unlock(&g_poll_lock);
    irq_restore(flags);
    p->wakeup_tick = 0;
    return true;
}

static uint32_t add_fd_objects(int fd, void **objects, uint32_t count, bool *wildcard) {
    void *found[2];
    int n = fd_poll_objects(fd, found, 2);
    for (int i = 0; i < n; i++) {
        bool duplicate = false;
        for (uint32_t j = 0; j < count; j++) duplicate |= objects[j] == found[i];
        if (duplicate) continue;
        if (count == POLL_WATCH_MAX) {
            *wildcard = true;
            return count;
        }
        objects[count++] = found[i];
    }
    return count;
}

static uint64_t timeout_deadline(int timeout) {
    if (timeout < 0) return UINT64_MAX;
    uint64_t delta = (uint64_t) timeout;
    return delta < UINT64_MAX - g_ticks ? g_ticks + delta : UINT64_MAX - 1u;
}

static uint64_t timespec_deadline(uint64_t sec, uint64_t subsec, uint64_t divisor) {
    if (sec > (UINT64_MAX - 1u) / 1000u) return UINT64_MAX - 1u;
    uint64_t delta = sec * 1000u + subsec / divisor;
    if (delta >= UINT64_MAX - g_ticks) return UINT64_MAX - 1u;
    return g_ticks + delta;
}

static uint64_t poll_fd_deadline(const struct pollfd_s *fds, uint64_t nfds,
                                 uint64_t deadline) {
    for (uint64_t i = 0; i < nfds; i++) {
        if (fds[i].fd < 0) continue;
        uint64_t fd_deadline = fd_poll_deadline(fds[i].fd);
        if (fd_deadline < deadline) deadline = fd_deadline;
    }
    return deadline;
}

static int poll_check(struct pollfd_s *fds, uint64_t nfds) {
    int ready = 0;
    for (uint64_t i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) continue;
        if (!fd_valid(fds[i].fd)) {
            fds[i].revents = POLLNVAL;
        } else {
            if ((fds[i].events & POLLIN) && fd_pollin(fds[i].fd)) fds[i].revents |= POLLIN;
            if ((fds[i].events & POLLOUT) && fd_pollout(fds[i].fd)) fds[i].revents |= POLLOUT;
            if (fd_pollhup(fds[i].fd))
                fds[i].revents |= POLLHUP; // HUP reported regardless of events
        }
        if (fds[i].revents) ready++;
    }
    return ready;
}

typedef struct {
    struct pollfd_s *fds;
    uint64_t nfds;
} poll_wait_ctx_t;

static bool poll_wait_ready(void *arg) {
    poll_wait_ctx_t *ctx = (poll_wait_ctx_t *) arg;
    return poll_check(ctx->fds, ctx->nfds) > 0;
}

int64_t sys_poll(struct pollfd_s *fds, uint64_t nfds, int timeout) {
    if (!fds && nfds) return -(int64_t) EFAULT;
    if (nfds > VFS_FD_MAX) return -(int64_t) EINVAL;
    if (nfds && !uptr_ok_w(fds, nfds * sizeof(*fds))) return -(int64_t) EFAULT;
    int ready = nfds ? poll_check(fds, nfds) : 0;
    if (ready > 0 || timeout == 0) return ready;
    proc_t *p = g_current_proc;
    uint64_t deadline = timeout_deadline(timeout);
    poll_wait_ctx_t ctx = { fds, nfds };
    void *objects[POLL_WATCH_MAX];
    uint32_t object_count = 0;
    bool wildcard = false;
    for (uint64_t i = 0; i < nfds && !wildcard; i++)
        if (fds[i].fd >= 0)
            object_count = add_fd_objects(fds[i].fd, objects, object_count, &wildcard);
    while (!ready) {
        if (deadline != UINT64_MAX && g_ticks >= deadline) break;
        if (p && (p->pending_sigs & ~p->sig_mask)) return -(int64_t) EINTR;
        uint64_t wait_deadline = poll_fd_deadline(fds, nfds, deadline);
        poll_wait_once(wait_deadline, poll_wait_ready, &ctx, objects, object_count, wildcard);
        if (nfds) ready = poll_check(fds, nfds);
    }
    return (int64_t) ready;
}

int64_t sys_ppoll(struct pollfd_s *fds, uint64_t nfds, void *tmo, const void *sigmask,
                  uint64_t sigsetsize) {
    (void) sigmask;
    (void) sigsetsize;
    int timeout = -1;
    if (tmo) {
        if (!uptr_ok(tmo, 16)) return -(int64_t) EFAULT;
        uint64_t ms = ((uint64_t *) tmo)[0] * 1000 + ((uint64_t *) tmo)[1] / 1000000;
        timeout = ms > 0x7fffffff ? 0x7fffffff : (int) ms;
    }
    return sys_poll(fds, nfds, timeout);
}

static inline bool fds_test(const uint8_t *set, int fd) {
    return set && ((set[fd >> 3] >> (fd & 7)) & 1);
}

static inline void fds_set(uint8_t *set, int fd) {
    if (set) set[fd >> 3] |= (uint8_t) (1 << (fd & 7));
}

typedef struct {
    int nfds;
    const uint8_t *rfds;
    const uint8_t *wfds;
    uint8_t *rout;
    uint8_t *wout;
    int ready;
} select_wait_ctx_t;

static bool select_check(void *arg) {
    select_wait_ctx_t *ctx = (select_wait_ctx_t *) arg;
    memset(ctx->rout, 0, 128);
    memset(ctx->wout, 0, 128);
    ctx->ready = 0;
    for (int fd = 0; fd < ctx->nfds; fd++) {
        bool fd_ready = false;
        if (fds_test(ctx->rfds, fd) && fd_pollin(fd)) {
            fds_set(ctx->rout, fd);
            fd_ready = true;
        }
        if (fds_test(ctx->wfds, fd) && fd_pollout(fd)) {
            fds_set(ctx->wout, fd);
            fd_ready = true;
        }
        if (fd_ready) ctx->ready++;
    }
    return ctx->ready > 0;
}

static int64_t sys_select_common(int nfds, void *rfds, void *wfds, void *efds, void *timeout,
                                 bool timeout_is_user) {
    if (nfds < 0 || nfds > VFS_FD_MAX) return -(int64_t) EINVAL;
    uint64_t set_bytes = ((uint64_t) nfds + 7) / 8;
    if (rfds && (!uptr_ok(rfds, set_bytes) || !uptr_ok_w(rfds, set_bytes)))
        return -(int64_t) EFAULT;
    if (wfds && (!uptr_ok(wfds, set_bytes) || !uptr_ok_w(wfds, set_bytes)))
        return -(int64_t) EFAULT;
    if (efds && (!uptr_ok(efds, set_bytes) || !uptr_ok_w(efds, set_bytes)))
        return -(int64_t) EFAULT;
    if (timeout && timeout_is_user && !uptr_ok(timeout, 16)) return -(int64_t) EFAULT;
    uint64_t deadline = (uint64_t) -1ULL;
    if (timeout) {
        deadline =
            timespec_deadline(((uint64_t *) timeout)[0], ((uint64_t *) timeout)[1], 1000u);
    }
    uint8_t rout[128], wout[128];
    proc_t *p = g_current_proc;
    select_wait_ctx_t ctx = { nfds, (const uint8_t *) rfds, (const uint8_t *) wfds,
                              rout, wout, 0 };
    void *objects[POLL_WATCH_MAX];
    uint32_t object_count = 0;
    bool wildcard = false;
    for (int fd = 0; fd < nfds && !wildcard; fd++)
        if (fds_test((const uint8_t *) rfds, fd) || fds_test((const uint8_t *) wfds, fd))
            object_count = add_fd_objects(fd, objects, object_count, &wildcard);
    for (;;) {
        select_check(&ctx);
        int ready = ctx.ready;
        if (ready > 0 || g_ticks >= deadline) {
            if (rfds) memcpy(rfds, rout, set_bytes);
            if (wfds) memcpy(wfds, wout, set_bytes);
            if (efds) memset(efds, 0, set_bytes);
            return (int64_t) ready;
        }
        if (p && (p->pending_sigs & ~p->sig_mask)) return -(int64_t) EINTR;
        uint64_t wait_deadline = deadline;
        for (int fd = 0; fd < nfds; fd++) {
            if (!fds_test((const uint8_t *) rfds, fd) &&
                !fds_test((const uint8_t *) wfds, fd))
                continue;
            uint64_t fd_deadline = fd_poll_deadline(fd);
            if (fd_deadline < wait_deadline) wait_deadline = fd_deadline;
        }
        poll_wait_once(wait_deadline, select_check, &ctx, objects, object_count, wildcard);
    }
}

int64_t sys_select(int nfds, void *rfds, void *wfds, void *efds, void *timeout) {
    return sys_select_common(nfds, rfds, wfds, efds, timeout, true);
}

int64_t sys_pselect6(int nfds, void *rfds, void *wfds, void *efds, void *timeout, void *sigmask) {
    (void) sigmask;
    uint64_t tv[2] = { 0, 0 };
    if (timeout) {
        if (!uptr_ok(timeout, 16)) return -(int64_t) EFAULT;
        tv[0] = ((uint64_t *) timeout)[0];
        tv[1] = ((uint64_t *) timeout)[1] / 1000;
    }
    return sys_select_common(nfds, rfds, wfds, efds, timeout ? tv : NULL, false);
}
