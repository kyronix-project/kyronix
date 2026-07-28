#include "pipe.h"
#include "fs/vfs_internal.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "syscall/poll.h"
#include <stdbool.h>

#define PIPE_MAGIC 0x4b59504950454d47ULL
#define EPIPE 32
#define EAGAIN 11
#define EIO 5

pipe_t *pipe_alloc(void) {
    pipe_t *p = (pipe_t *) kcalloc(1, sizeof(pipe_t));
    if (p) {
        p->magic = PIPE_MAGIC;
        p->lock.lock = 0;
    }
    return p;
}

void pipe_free(pipe_t *p) {
    if (!p) return;
    spin_lock(&p->lock);
    uint64_t waiters = p->reader_waiters | p->writer_waiters;
    p->reader_waiters = 0;
    p->writer_waiters = 0;
    while (waiters) {
        int slot = __builtin_ctzll(waiters);
        proc_t *t = &g_proctable[slot];
        if (t->blocked_pipe == p) t->blocked_pipe = NULL;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
            proc_set_ready(t);
        waiters &= waiters - 1;
    }
    while (p->anc_rd != p->anc_wr) {
        pipe_anc_t *slot = &p->anc_q[p->anc_rd];
        for (int i = 0; i < slot->nfds; i++)
            vfs_file_close((vfs_file_t *) slot->files[i]);
        p->anc_rd = (p->anc_rd + 1) % PIPE_ANC_SLOTS;
    }
    p->magic = 0;
    spin_unlock(&p->lock);
    kfree(p);
    poll_notify();
}

static bool pipe_valid(pipe_t *p) {
    uintptr_t addr = (uintptr_t) p;
    if (!p || addr < HEAP_START || addr >= HEAP_MAX || (addr & 7)) return false;
    return p->magic == PIPE_MAGIC;
}

/* wake every proc blocked on this pipe in one direction (want_read=1 -> readers) */
void pipe_wake(pipe_t *p, int want_read) {
    spin_lock(&p->lock);
    uint64_t *mask = want_read ? &p->reader_waiters : &p->writer_waiters;
    uint64_t waiters = *mask;
    *mask = 0;
    while (waiters) {
        int slot = __builtin_ctzll(waiters);
        proc_t *t = &g_proctable[slot];
        if (t->blocked_pipe == p && t->blocked_pipe_read == want_read &&
            __sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY))
            proc_set_ready(t);
        waiters &= waiters - 1;
    }
    spin_unlock(&p->lock);
    poll_notify();
}

void pipe_cancel_wait(pipe_t *p, void *proc) {
    proc_t *task = (proc_t *) proc;
    if (!p || !task || task->blocked_pipe != p) return;
    spin_lock(&p->lock);
    uint64_t bit = 1ULL << proc_slot(task);
    p->reader_waiters &= ~bit;
    p->writer_waiters &= ~bit;
    if (task->blocked_pipe == p) task->blocked_pipe = NULL;
    spin_unlock(&p->lock);
}

static void pipe_block_locked(pipe_t *p, proc_t *task, int want_read) {
    task->blocked_pipe = p;
    task->blocked_pipe_read = want_read;
    task->state = PROC_WAITING;
    uint64_t bit = 1ULL << proc_slot(task);
    if (want_read)
        p->reader_waiters |= bit;
    else
        p->writer_waiters |= bit;
}

int64_t pipe_read(pipe_t *p, void *buf, uint64_t len) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            if (p->count == 0) {
                if (p->write_refs == 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }
                if (done > 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }

                proc_t *_rp = g_current_proc;
                if (_rp) pipe_block_locked(p, _rp, 1);
                spin_unlock(&p->lock);
                if (_rp) sched_block_current();
                pipe_cancel_wait(p, _rp);
                goto restart_read;
            }
            uint64_t take = len - done;
            if (take > p->count) take = p->count;
            uint64_t first = take;
            if (first > PIPE_BUFSZ - p->rpos) first = PIPE_BUFSZ - p->rpos;
            memcpy(out + done, p->buf + p->rpos, first);
            if (take > first) memcpy(out + done + first, p->buf, take - first);
            p->rpos = (p->rpos + (uint32_t) take) % PIPE_BUFSZ;
            p->count -= (uint32_t) take;
            done += take;
        }
        spin_unlock(&p->lock);
        break;
    restart_read:;
    }

    pipe_wake(p, 0); /* space freed: wake all blocked writers */
    return (int64_t) done;
}

int64_t pipe_peek(pipe_t *p, void *buf, uint64_t len, uint64_t skip) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            if (p->count <= skip + done) {
                if (p->write_refs == 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }
                if (done > 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }

                proc_t *_rp = g_current_proc;
                if (_rp) pipe_block_locked(p, _rp, 1);
                spin_unlock(&p->lock);
                if (_rp) sched_block_current();
                pipe_cancel_wait(p, _rp);
                goto restart_peek;
            }

            uint64_t available = p->count - skip - done;
            uint64_t take = len - done;
            if (take > available) take = available;
            uint32_t pos = (p->rpos + (uint32_t) skip + (uint32_t) done) % PIPE_BUFSZ;
            uint64_t first = take;
            if (first > PIPE_BUFSZ - pos) first = PIPE_BUFSZ - pos;
            memcpy(out + done, p->buf + pos, first);
            if (take > first) memcpy(out + done + first, p->buf, take - first);
            done += take;
        }
        spin_unlock(&p->lock);
        break;
    restart_peek:;
    }

    return (int64_t) done;
}

int64_t pipe_write(pipe_t *p, const void *buf, uint64_t len) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    if (p->read_refs == 0) {
        proc_send_signal(g_current_proc, SIGPIPE);
        return -(int64_t) EPIPE;
    }
    if (len == 0) return 0;

    const uint8_t *in = (const uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            while (p->count == PIPE_BUFSZ) {
                if (p->read_refs == 0) {
                    spin_unlock(&p->lock);
                    proc_send_signal(g_current_proc, SIGPIPE);
                    return done ? (int64_t) done : -(int64_t) EPIPE;
                }

                proc_t *_wp = g_current_proc;
                if (_wp) pipe_block_locked(p, _wp, 0);
                spin_unlock(&p->lock);
                pipe_wake(p, 1); /* let readers drain so space frees up */
                if (_wp) sched_block_current();
                pipe_cancel_wait(p, _wp);
                goto restart_write;
            }
            uint32_t wpos = (p->rpos + p->count) % PIPE_BUFSZ;
            uint64_t put = len - done;
            uint64_t space = PIPE_BUFSZ - p->count;
            if (put > space) put = space;
            uint64_t first = put;
            if (first > PIPE_BUFSZ - wpos) first = PIPE_BUFSZ - wpos;
            memcpy(p->buf + wpos, in + done, first);
            if (put > first) memcpy(p->buf, in + done + first, put - first);
            p->count += (uint32_t) put;
            done += put;
        }
        spin_unlock(&p->lock);
        break;
    restart_write:;
    }

    pipe_wake(p, 1); /* data available: wake all blocked readers */
    return (int64_t) done;
}

int pipe_anc_send(pipe_t *p, void **files, int nfds) {
    if (!pipe_valid(p) || !files || nfds <= 0 || nfds > PIPE_ANC_MAXFDS) return -1;
    spin_lock(&p->lock);
    uint32_t next = (p->anc_wr + 1) % PIPE_ANC_SLOTS;
    if (next == p->anc_rd) {
        spin_unlock(&p->lock);
        return -1;
    }
    pipe_anc_t *slot = &p->anc_q[p->anc_wr];
    slot->nfds = nfds;
    for (int i = 0; i < nfds; i++) slot->files[i] = files[i];
    p->anc_wr = next;
    spin_unlock(&p->lock);
    return 0;
}

int pipe_anc_recv(pipe_t *p, void **out, int max) {
    if (!pipe_valid(p) || !out || max <= 0) return 0;
    spin_lock(&p->lock);
    if (p->anc_rd == p->anc_wr) {
        spin_unlock(&p->lock);
        return 0;
    }
    pipe_anc_t *slot = &p->anc_q[p->anc_rd];
    int n = slot->nfds < max ? slot->nfds : max;
    for (int i = 0; i < n; i++) out[i] = slot->files[i];
    p->anc_rd = (p->anc_rd + 1) % PIPE_ANC_SLOTS;
    spin_unlock(&p->lock);
    return n;
}
