#include "eventfd.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs_internal.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "syscall/poll.h"

extern volatile uint64_t g_ticks;

#define EAGAIN 11
#define EINVAL 22
#define EMFILE 24
#define ENOMEM 12

int fd_eventfd(uint32_t initval, int eflags) {
    eventfd_state_t *e = (eventfd_state_t *) kcalloc(1, sizeof(eventfd_state_t));
    if (!e) return -(int) ENOMEM;
    e->counter = initval;
    e->semaphore = !!(eflags & 1);
    e->refcnt = 1;
    e->lock.lock = 0;

    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        kfree(e);
        return -(int) EMFILE;
    }
    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(fd);
        kfree(e);
        return -(int) ENOMEM;
    }
    f->efd = e;
    f->flags = O_RDWR;
    if (eflags & 0x80000) f->flags |= O_NONBLOCK;
    if (eflags & 0x40000) f->cloexec = 1;
    vfs_fd_install(fd, f);
    return fd;
}

int64_t eventfd_read(vfs_file_t *f, char *buf, uint64_t len) {
    if (len < 8) return -(int) EINVAL;
    eventfd_state_t *e = f->efd;

    for (;;) {
        spin_lock(&e->lock);
        if (e->counter != 0) {
            uint64_t val;
            if (e->semaphore) {
                val = 1;
                e->counter--;
            } else {
                val = e->counter;
                e->counter = 0;
            }
            spin_unlock(&e->lock);
            poll_notify_object(e);
            __builtin_memcpy(buf, &val, 8);
            return 8;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&e->lock);
            return -(int) EAGAIN;
        }
        proc_t *p = g_current_proc;
        e->waiter = p;
        if (p) p->state = PROC_WAITING;
        spin_unlock(&e->lock);

        if (p) sched_block_current();
        spin_lock(&e->lock);
        if (e->waiter == p) e->waiter = NULL;
        spin_unlock(&e->lock);
    }
}

int64_t eventfd_write(vfs_file_t *f, const char *buf, uint64_t len) {
    if (len < 8) return -(int) EINVAL;
    uint64_t val;
    __builtin_memcpy(&val, buf, 8);
    if (val == (uint64_t) -1) return -(int) EINVAL;
    eventfd_state_t *e = f->efd;

    spin_lock(&e->lock);
    e->counter += val;
    if (e->waiter) {
        proc_t *w = (proc_t *) e->waiter;
        if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
        e->waiter = NULL;
    }
    spin_unlock(&e->lock);
    poll_notify_object(e);
    return 8;
}

int fd_timerfd_create(int clockid, int tflags) {
    timerfd_state_t *t = (timerfd_state_t *) kcalloc(1, sizeof(timerfd_state_t));
    if (!t) return -(int) ENOMEM;
    t->clockid = clockid;
    t->refcnt = 1;
    t->lock.lock = 0;

    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        kfree(t);
        return -(int) EMFILE;
    }
    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(fd);
        kfree(t);
        return -(int) ENOMEM;
    }
    f->tfd = t;
    f->flags = O_RDWR;
    if (tflags & 0x80000) f->flags |= O_NONBLOCK;
    if (tflags & 0x40000) f->cloexec = 1;
    vfs_fd_install(fd, f);
    return fd;
}

int fd_timerfd_settime(int fd, int flags, const kitimerspec_t *new_val, kitimerspec_t *old_val) {
    (void) flags;
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->tfd) return -(int) EINVAL;
    timerfd_state_t *t = f->tfd;
    spin_lock(&t->lock);
    if (old_val) {
        uint64_t remaining_ms = (t->next_tick > g_ticks) ? (t->next_tick - g_ticks) : 0;
        old_val->value.sec = remaining_ms / 1000;
        old_val->value.nsec = (remaining_ms % 1000) * 1000000;
        old_val->interval.sec = t->interval_ms / 1000;
        old_val->interval.nsec = (t->interval_ms % 1000) * 1000000;
    }
    uint64_t val_ms = new_val->value.sec * 1000 + new_val->value.nsec / 1000000;
    t->interval_ms = new_val->interval.sec * 1000 + new_val->interval.nsec / 1000000;
    t->next_tick = val_ms ? g_ticks + val_ms : 0;
    t->overruns = 0;
    proc_t *waiter = (proc_t *) t->waiter;
    t->waiter = NULL;
    if (waiter && __sync_bool_compare_and_swap(&waiter->state, PROC_WAITING, PROC_READY))
        proc_set_ready(waiter);
    spin_unlock(&t->lock);
    poll_notify_object(t);
    return 0;
}

int fd_timerfd_gettime(int fd, kitimerspec_t *cur_val) {
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->tfd || !cur_val) return -(int) EINVAL;
    timerfd_state_t *t = f->tfd;
    spin_lock(&t->lock);
    uint64_t remaining_ms = (t->next_tick > g_ticks) ? (t->next_tick - g_ticks) : 0;
    cur_val->value.sec = remaining_ms / 1000;
    cur_val->value.nsec = (remaining_ms % 1000) * 1000000;
    cur_val->interval.sec = t->interval_ms / 1000;
    cur_val->interval.nsec = (t->interval_ms % 1000) * 1000000;
    spin_unlock(&t->lock);
    return 0;
}

int64_t timerfd_read(vfs_file_t *f, char *buf, uint64_t len) {
    if (len < 8) return -(int) EINVAL;
    timerfd_state_t *t = f->tfd;
    for (;;) {
        spin_lock(&t->lock);
        if (t->next_tick && g_ticks >= t->next_tick) {
            uint64_t exp = 1 + t->overruns;
            t->overruns = 0;
            if (t->interval_ms)
                t->next_tick += t->interval_ms;
            else
                t->next_tick = 0;
            spin_unlock(&t->lock);
            __builtin_memcpy(buf, &exp, 8);
            return 8;
        }
        if (f->flags & O_NONBLOCK) {
            spin_unlock(&t->lock);
            return -(int) EAGAIN;
        }
        proc_t *p = g_current_proc;
        if (p) {
            t->waiter = p;
            if (t->next_tick) {
                p->wakeup_tick = t->next_tick;
                proc_set_timer(p);
            }
            p->state = PROC_WAITING;
        }
        spin_unlock(&t->lock);
        if (p) sched_block_current();
        spin_lock(&t->lock);
        if (t->waiter == p) t->waiter = NULL;
        spin_unlock(&t->lock);
        if (p) p->wakeup_tick = 0;
    }
}
