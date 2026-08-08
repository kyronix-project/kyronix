#include "epoll.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/poll.h"
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
#define EPOLLONESHOT 0x40000000U
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
    vmm_space_t *owner_space;
    struct epoll_watch w[EPOLL_MAXW];
    int nw;
} epoll_t;

static epoll_t g_epolls[EPOLL_SLOTS];
static int g_epoll_init;
static spinlock_t g_epolls_lock;

static bool epoll_ready(void *arg) {
    epoll_t *ep = (epoll_t *) arg;
    for (int i = 0; i < ep->nw; i++) {
        int fd = ep->w[i].fd;
        if (!fd_valid(fd)) return true;
        if ((ep->w[i].events & EPOLLIN) && fd_pollin(fd)) return true;
        if ((ep->w[i].events & EPOLLOUT) && fd_pollout(fd)) return true;
        if (fd_pollhup(fd)) return true;
    }
    return false;
}

static uint64_t epoll_wait_deadline(epoll_t *ep, uint64_t deadline) {
    for (int i = 0; i < ep->nw; i++) {
        uint64_t fd_deadline = fd_poll_deadline(ep->w[i].fd);
        if (fd_deadline < deadline) deadline = fd_deadline;
    }
    return deadline;
}

static uint32_t epoll_objects(epoll_t *ep, void **objects, uint32_t max, bool *wildcard) {
    uint32_t count = 0;
    for (int i = 0; i < ep->nw; i++) {
        void *found[2];
        int n = fd_poll_objects(ep->w[i].fd, found, 2);
        for (int j = 0; j < n; j++) {
            bool duplicate = false;
            for (uint32_t k = 0; k < count; k++) duplicate |= objects[k] == found[j];
            if (duplicate) continue;
            if (count == max) {
                *wildcard = true;
                return count;
            }
            objects[count++] = found[j];
        }
    }
    return count;
}

static epoll_t *epoll_find(int epfd) {
    proc_t *p = g_current_proc;
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == epfd && g_epolls[i].owner_space == (p ? p->space : NULL))
            return &g_epolls[i];
    return NULL;
}

int64_t sys_epoll_create1(int flags) {
    (void) flags;
    proc_t *p = g_current_proc;
    if (!g_epoll_init) {
        for (int i = 0; i < EPOLL_SLOTS; i++) {
            g_epolls[i].epfd = -1;
            g_epolls[i].owner_space = NULL;
        }
        g_epoll_init = 1;
    }
    spin_lock(&g_epolls_lock);
    for (int i = 0; i < EPOLL_SLOTS; i++) {
        if (g_epolls[i].epfd >= 0 && g_epolls[i].owner_space == (p ? p->space : NULL) &&
            !fd_valid(g_epolls[i].epfd)) {
            g_epolls[i].epfd = -1;
            g_epolls[i].owner_space = NULL;
            g_epolls[i].nw = 0;
        }
    }
    int epfd =
        fd_open_host("/dev/null", O_RDONLY, 0); // internal handle: not subject to jail root
    if (epfd < 0) {
        spin_unlock(&g_epolls_lock);
        return -(int64_t) EMFILE;
    }
    epoll_t *ep = NULL;
    for (int i = 0; i < EPOLL_SLOTS; i++)
        if (g_epolls[i].epfd == -1) {
            ep = &g_epolls[i];
            break;
        }
    if (!ep) {
        spin_unlock(&g_epolls_lock);
        fd_close(epfd);
        return -(int64_t) ENOMEM;
    }
    ep->epfd = epfd;
    ep->owner_space = p ? p->space : NULL;
    ep->nw = 0;
    spin_unlock(&g_epolls_lock);
    return epfd;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    epoll_t *ep = epoll_find(epfd);
    if (!ep) return -(int64_t) EBADF;
    if (op != EPOLL_CTL_DEL && ev && !uptr_ok(ev, sizeof(*ev))) return -(int64_t) EFAULT;
    if (op != EPOLL_CTL_DEL && !fd_valid(fd)) return -(int64_t) EBADF;
    spin_lock(&g_epolls_lock);
    switch (op) {
    case EPOLL_CTL_ADD:
        if (ep->nw >= EPOLL_MAXW) {
            spin_unlock(&g_epolls_lock);
            return -(int64_t) ENOMEM;
        }
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) {
                spin_unlock(&g_epolls_lock);
                return -(int64_t) EEXIST;
            }
        ep->w[ep->nw] =
            (struct epoll_watch) { fd, ev ? ev->events : EPOLLIN | EPOLLOUT, ev ? ev->data : 0 };
        ep->nw++;
        spin_unlock(&g_epolls_lock);
        return 0;
    case EPOLL_CTL_DEL:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) {
                ep->w[i] = ep->w[--ep->nw];
                spin_unlock(&g_epolls_lock);
                return 0;
            }
        spin_unlock(&g_epolls_lock);
        return -(int64_t) ENOENT;
    case EPOLL_CTL_MOD:
        for (int i = 0; i < ep->nw; i++)
            if (ep->w[i].fd == fd) {
                ep->w[i].events = ev ? ev->events : 0;
                ep->w[i].data = ev ? ev->data : 0;
                spin_unlock(&g_epolls_lock);
                return 0;
            }
        spin_unlock(&g_epolls_lock);
        return -(int64_t) ENOENT;
    }
    spin_unlock(&g_epolls_lock);
    return -(int64_t) EINVAL;
}

int64_t sys_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    epoll_t *ep = epoll_find(epfd);
    if (!ep || !events || maxevents <= 0) return -(int64_t) EINVAL;
    if (!uptr_ok_w(events, (uint64_t) maxevents * sizeof(*events))) return -(int64_t) EFAULT;
    proc_t *p = g_current_proc;
    uint64_t deadline = UINT64_MAX;
    if (timeout >= 0) {
        uint64_t delta = (uint64_t) timeout;
        deadline = delta < UINT64_MAX - g_ticks ? g_ticks + delta : UINT64_MAX - 1u;
    }
    for (;;) {
        int n = 0;
        for (int i = 0; i < ep->nw && n < maxevents; i++) {
            int wfd = ep->w[i].fd;
            uint32_t got = fd_valid(wfd) ? 0 : (EPOLLERR | EPOLLHUP);
            if (!got) {
                if ((ep->w[i].events & EPOLLIN) && fd_pollin(wfd)) got |= EPOLLIN;
                if ((ep->w[i].events & EPOLLOUT) && fd_pollout(wfd)) got |= EPOLLOUT;
                if (fd_pollhup(wfd)) got |= EPOLLHUP; // HUP reported regardless of interest
            }
            if (got) {
                events[n].events = got;
                events[n].data = ep->w[i].data;
                n++;
                if (ep->w[i].events & EPOLLONESHOT)
                    ep->w[i].events &= ~(EPOLLIN | EPOLLOUT); // disarm until re-armed via MOD
            }
        }
        if (n > 0 || timeout == 0 || (deadline != UINT64_MAX && g_ticks >= deadline)) return n;
        if (p && (p->pending_sigs & ~p->sig_mask)) return -(int64_t) EINTR;
        void *objects[64];
        bool wildcard = false;
        uint32_t object_count = epoll_objects(ep, objects, 64, &wildcard);
        poll_wait_once(epoll_wait_deadline(ep, deadline), epoll_ready, ep,
                       objects, object_count, wildcard);
    }
}
