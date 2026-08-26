#include "signalfd.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs_internal.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "syscall/poll.h"

#define EAGAIN 11
#define EINVAL 22
#define EMFILE 24
#define ENOMEM 12
#define EBADF 9
#define EINTR 4

#define SFD_NONBLOCK 04000
#define SFD_CLOEXEC 02000000

#define SIGINFO_SIZE 128

int fd_signalfd(int fd, uint64_t mask, int flags) {
    if (flags & ~(SFD_NONBLOCK | SFD_CLOEXEC)) return -(int) EINVAL;

    // SIGKILL and SIGSTOP can never be caught, and neither can they be read
    mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    if (fd >= 0) {
        vfs_file_t *f = vfs_fd_get(fd);
        if (!f || !f->sfd) return -(int) EINVAL;
        spin_lock(&f->sfd->lock);
        f->sfd->mask = mask;
        spin_unlock(&f->sfd->lock);
        return fd;
    }

    sigfd_state_t *s = (sigfd_state_t *) kcalloc(1, sizeof(sigfd_state_t));
    if (!s) return -(int) ENOMEM;
    s->mask = mask;
    s->refcnt = 1;

    int newfd = vfs_fd_alloc_from(0);
    if (newfd < 0) {
        kfree(s);
        return -(int) EMFILE;
    }
    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(newfd);
        kfree(s);
        return -(int) ENOMEM;
    }
    f->sfd = s;
    f->flags = O_RDONLY;
    if (flags & SFD_NONBLOCK) f->flags |= O_NONBLOCK;
    if (flags & SFD_CLOEXEC) f->cloexec = 1;
    vfs_fd_install(newfd, f);
    return newfd;
}

bool signalfd_pollin(vfs_file_t *f) {
    proc_t *p = g_current_proc;
    if (!f || !f->sfd || !p) return false;
    return (__atomic_load_n(&p->pending_sigs, __ATOMIC_RELAXED) & f->sfd->mask) != 0;
}

static void fill_siginfo(char *buf, int sig) {
    memset(buf, 0, SIGINFO_SIZE);
    uint32_t *u32 = (uint32_t *) buf;
    u32[0] = (uint32_t) sig; // ssi_signo
    u32[3] = g_current_proc ? g_current_proc->pid : 0;
    u32[4] = g_current_proc ? g_current_proc->uid : 0;
}

int64_t signalfd_read(vfs_file_t *f, char *buf, uint64_t len) {
    if (!f || !f->sfd) return -(int64_t) EBADF;
    if (len < SIGINFO_SIZE) return -(int64_t) EINVAL;
    proc_t *p = g_current_proc;
    if (!p) return -(int64_t) EINVAL;

    for (;;) {
        uint64_t bits = __atomic_load_n(&p->pending_sigs, __ATOMIC_RELAXED) & f->sfd->mask;
        if (bits) {
            int sig = __builtin_ctzll(bits) + 1;
            __atomic_fetch_and(&p->pending_sigs, ~(1ULL << (sig - 1)), __ATOMIC_RELAXED);
            fill_siginfo(buf, sig);
            return SIGINFO_SIZE;
        }
        if (f->flags & O_NONBLOCK) return -(int64_t) EAGAIN;
        // an unblocked signal must interrupt the read instead
        if (p->pending_sigs & ~p->sig_mask) return -(int64_t) EINTR;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
    }
}
