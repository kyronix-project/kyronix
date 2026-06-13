#include "epoll.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EBADF 9
#define EEXIST 17
#define EFAULT 14
#define EINVAL 22
#define EINTR 4
#define EMFILE 24
#define ENOENT 2
#define ENOMEM 12

#define EPOLLIN 0x001U
#define EPOLLOUT 0x004U
#define EPOLLERR 0x008U
#define EPOLLHUP 0x010U
#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3
#define EPOLL_SLOTS 64
#define EPOLL_MAXW 256

extern volatile uint64_t g_ticks;

struct epoll_watch {
    int fd;
    uint32_t events;
    uint64_t data;
};

typedef struct {
    int epfd;
    vmm_space_t* owner_space;
    struct epoll_watch w[EPOLL_MAXW];
    int nw;
} epoll_t;

static epoll_t g_epolls[EPOLL_SLOTS];
static int g_epoll_init;

static epoll_t* epoll_find(int epfd)
{
    proc_t* p = g_current_proc;
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == epfd && g_epolls[i].owner_space == (p ? p->space : NULL))
            return &g_epolls[i];
    return NULL;
}

int64_t sys_epoll_create1(int flags)
{
    (void) flags;
    proc_t* p = g_current_proc;
    if (!g_epoll_init) {
        for (int i = 0; i < EPOLL_SLOTS; i++) {
            g_epolls[i].epfd = -1;
            g_epolls[i].owner_space = NULL;
        }
        g_epoll_init = 1;
    }
    for (int i = 0; i < EPOLL_SLOTS; i++) {
        if (g_epolls[i].epfd >= 0 && g_epolls[i].owner_space == (p ? p->space : NULL) &&
            !fd_valid(g_epolls[i].epfd)) {
            g_epolls[i].epfd = -1;
            g_epolls[i].owner_space = NULL;
            g_epolls[i].nw = 0;
        }
    }
    int epfd = fd_open("/dev/null", O_RDONLY, 0);
    if (epfd < 0)
        return -(int64_t) EMFILE;
    epoll_t* ep = NULL;
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == -1) {
            ep = &g_epolls[i];
            break;
        }
    if (!ep) {
        fd_close(epfd);
        return -(int64_t) ENOMEM;
    }
    ep->epfd = epfd;
    ep->owner_space = p ? p->space : NULL;
    ep->nw = 0;
    return epfd;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event* ev)
{
    epoll_t* ep = epoll_find(epfd);
    if (!ep)
        return -(int64_t) EBADF;
    if (op != EPOLL_CTL_DEL && ev && !uptr_ok(ev, sizeof(*ev)))
        return -(int64_t) EFAULT;
    if (op != EPOLL_CTL_DEL && !fd_valid(fd))
        return -(int64_t) EBADF;
    switch (op) {
    case EPOLL_CTL_ADD:
        if (ep->nw >= EPOLL_MAXW)
            return -(int64_t) ENOMEM;
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd)
                return -(int64_t) EEXIST;
        ep->w[ep->nw] =
            (struct epoll_watch) {fd, ev ? ev->events : EPOLLIN | EPOLLOUT, ev ? ev->data : 0};
        ep->nw++;
        return 0;
    case EPOLL_CTL_DEL:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) {
                ep->w[i] = ep->w[--ep->nw];
                return 0;
            }
        return -(int64_t) ENOENT;
    case EPOLL_CTL_MOD:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) {
                ep->w[i].events = ev ? ev->events : 0;
                ep->w[i].data = ev ? ev->data : 0;
                return 0;
            }
        return -(int64_t) ENOENT;
    }
    return -(int64_t) EINVAL;
}

int64_t sys_epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout)
{
    epoll_t* ep = epoll_find(epfd);
    if (!ep || !events || maxevents <= 0)
        return -(int64_t) EINVAL;
    if (!uptr_ok_w(events, (uint64_t) maxevents * sizeof(*events)))
        return -(int64_t) EFAULT;
    proc_t* p = g_current_proc;
    uint64_t deadline = timeout >= 0 ? g_ticks + (uint64_t) timeout : (uint64_t) -1ULL;
    for (;;) {
        int n = 0;
        for (int i = 0; i < ep->nw && n < maxevents; i++) {
            int wfd = ep->w[i].fd;
            uint32_t got = fd_valid(wfd) ? 0 : (EPOLLERR | EPOLLHUP);
            if (!got) {
                if ((ep->w[i].events & EPOLLIN) && fd_pollin(wfd))
                    got |= EPOLLIN;
                if ((ep->w[i].events & EPOLLOUT) && fd_pollout(wfd))
                    got |= EPOLLOUT;
            }
            if (got) {
                events[n].events = got;
                events[n].data = ep->w[i].data;
                n++;
            }
        }
        if (n > 0 || timeout == 0 || g_ticks >= deadline)
            return n;
        if (p && (p->pending_sigs & ~p->sig_mask))
            return -(int64_t) EINTR;
        if (p)
            p->wakeup_tick = g_ticks + 5;
        if (proc_next_ready(p))
            sched_yield_blocking();
        else {
            sti();
            hlt();
            cli();
        }
        if (p)
            p->wakeup_tick = 0;
    }
}
