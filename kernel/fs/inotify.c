#include "fs/inotify.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "syscall/poll.h"

#define EAGAIN 11
#define EBADF 9
#define EEXIST 17
#define EFAULT 14
#define EINTR 4
#define EINVAL 22
#define EMFILE 24
#define ENOMEM 12
#define ENOTDIR 20
#define ENOENT 2

#define INOTIFY_MAGIC 0x4b59494e4f544659ULL
#define INOTIFY_QUEUE_MAX 4096u

struct inotify_event {
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
};

struct inotify_watch;

typedef struct inotify_entry {
    struct inotify_entry *next;
    int32_t wd;
    uint32_t mask;
    uint32_t cookie;
    uint32_t len;
    char name[];
} inotify_entry_t;

typedef struct inotify_watch {
    struct inotify_watch *next;
    vfs_node_t *node;
    int wd;
    uint32_t mask;
} inotify_watch_t;

typedef struct inotify_state {
    struct inotify_state *next;
    uint64_t magic;
    uint32_t refcnt;
    spinlock_t lock;
    inotify_watch_t *watches;
    inotify_entry_t *head;
    inotify_entry_t *tail;
    uint32_t count;
    int next_wd;
    uint8_t overflow;
} inotify_state_t;

static spinlock_t g_inotify_lock = SPINLOCK_INIT;
static inotify_state_t *g_inotify_states;
static uint32_t g_inotify_cookie;

static void inotify_queue(inotify_state_t *st, int wd, uint32_t mask, uint32_t cookie,
                          const char *name)
{
    uint32_t nlen = name ? (uint32_t) strlen(name) + 1u : 0;
    uint32_t len = (nlen + 3u) & ~3u;
    inotify_entry_t *e;

    if (st->count >= INOTIFY_QUEUE_MAX) {
        st->overflow = 1;
        return;
    }

    e = (inotify_entry_t *) kmalloc(sizeof(*e) + len);

    if (!e) {
        st->overflow = 1;
        return;
    }

    e->next = NULL;
    e->wd = wd;
    e->mask = mask;
    e->cookie = cookie;
    e->len = len;

    if (nlen) {
        memcpy(e->name, name, nlen);
        if (len > nlen) {
            memset(e->name + nlen, 0, len - nlen);
        }
    }

    if (st->tail) {
        st->tail->next = e;
    }
    else {
        st->head = e;
    }

    st->tail = e;
    st->count++;
}

static void inotify_watch_drop(inotify_state_t *st, inotify_watch_t **link)
{
    inotify_watch_t *w = *link;
    vfs_node_t *n = w->node;

    *link = w->next;
    kfree(w);
    vfs_node_unref_internal(n);
}

static void inotify_remove_watch(inotify_state_t *st, inotify_watch_t **link)
{
    inotify_watch_t *w = *link;

    inotify_queue(st, w->wd, IN_IGNORED, 0, NULL);
    inotify_watch_drop(st, link);
}

static void inotify_notify_state(inotify_state_t *st, vfs_node_t *n, bool parent, uint32_t mask,
                                 uint32_t cookie)
{
    inotify_watch_t **link;
    bool is_dir = n->type == VFS_TYPE_DIR;

    spin_lock(&st->lock);
    link = &st->watches;

    while (*link) {
        inotify_watch_t *w = *link;
        bool match = parent ? w->node == n->parent : w->node == n;

        if (!match) {
            link = &w->next;
            continue;
        }

        uint32_t got = mask & w->mask & IN_ALL_EVENTS;

        if (!got) {
            link = &w->next;
            continue;
        }

        if (is_dir) {
            got |= IN_ISDIR;
        }

        if (parent) {
            inotify_queue(st, w->wd, got, cookie, n->name);
        }
        else {
            inotify_queue(st, w->wd, got, cookie, NULL);
        }

        if (w->mask & IN_ONESHOT) {
            inotify_remove_watch(st, link);
            continue;
        }

        link = &w->next;
    }

    spin_unlock(&st->lock);
}

void inotify_notify_node(vfs_node_t *n, uint32_t mask, uint32_t cookie)
{
    if (!n) {
        return;
    }

    spin_lock(&g_inotify_lock);

    for (inotify_state_t *st = g_inotify_states; st; st = st->next) {
        inotify_notify_state(st, n, false, mask, cookie);
        inotify_notify_state(st, n, true, mask, cookie);
    }

    spin_unlock(&g_inotify_lock);
}

void inotify_notify_parent(vfs_node_t *parent, const char *name, bool is_dir, uint32_t mask,
                           uint32_t cookie)
{
    if (!parent || !name) {
        return;
    }

    spin_lock(&g_inotify_lock);

    for (inotify_state_t *st = g_inotify_states; st; st = st->next) {
        inotify_watch_t **link = &st->watches;

        spin_lock(&st->lock);

        while (*link) {
            inotify_watch_t *w = *link;

            if (w->node != parent) {
                link = &w->next;
                continue;
            }

            uint32_t got = mask & w->mask & IN_ALL_EVENTS;

            if (!got) {
                link = &w->next;
                continue;
            }

            if (is_dir) {
                got |= IN_ISDIR;
            }

            inotify_queue(st, w->wd, got, cookie, name);

            if (w->mask & IN_ONESHOT) {
                inotify_remove_watch(st, link);
                continue;
            }

            link = &w->next;
        }

        spin_unlock(&st->lock);
    }

    spin_unlock(&g_inotify_lock);
}

void inotify_notify_move(vfs_node_t *n, vfs_node_t *old_parent, const char *old_name,
                         vfs_node_t *new_parent, const char *new_name, uint32_t cookie)
{
    if (!n) {
        return;
    }

    bool is_dir = n->type == VFS_TYPE_DIR;
    spin_lock(&g_inotify_lock);

    for (inotify_state_t *st = g_inotify_states; st; st = st->next) {
        inotify_watch_t **link = &st->watches;

        spin_lock(&st->lock);

        while (*link) {
            inotify_watch_t *w = *link;

            if (w->node == old_parent && (w->mask & IN_MOVED_FROM)) {
                uint32_t got = IN_MOVED_FROM | (is_dir ? IN_ISDIR : 0);
                inotify_queue(st, w->wd, got, cookie, old_name);
            }

            if (w->node == new_parent && (w->mask & IN_MOVED_TO)) {
                uint32_t got = IN_MOVED_TO | (is_dir ? IN_ISDIR : 0);
                inotify_queue(st, w->wd, got, cookie, new_name);
            }

            if (w->node == n && (w->mask & IN_MOVE_SELF)) {
                uint32_t got = IN_MOVE_SELF | (is_dir ? IN_ISDIR : 0);
                inotify_queue(st, w->wd, got, 0, NULL);
            }

            if (w->mask & IN_ONESHOT) {
                inotify_remove_watch(st, link);
                continue;
            }

            link = &w->next;
        }

        spin_unlock(&st->lock);
    }

    spin_unlock(&g_inotify_lock);
}

void inotify_notify_delete_self(vfs_node_t *n)
{
    if (!n) {
        return;
    }

    bool is_dir = n->type == VFS_TYPE_DIR;
    spin_lock(&g_inotify_lock);

    for (inotify_state_t *st = g_inotify_states; st; st = st->next) {
        inotify_watch_t **link = &st->watches;

        spin_lock(&st->lock);

        while (*link) {
            inotify_watch_t *w = *link;

            if (w->node != n) {
                link = &w->next;
                continue;
            }

            if (w->mask & IN_DELETE_SELF) {
                uint32_t got = IN_DELETE_SELF | (is_dir ? IN_ISDIR : 0);
                inotify_queue(st, w->wd, got, 0, NULL);
            }

            inotify_remove_watch(st, link);
        }

        spin_unlock(&st->lock);
    }

    spin_unlock(&g_inotify_lock);
}

void inotify_state_addref(void *state)
{
    inotify_state_t *st = (inotify_state_t *) state;

    if (st) {
        __atomic_add_fetch(&st->refcnt, 1, __ATOMIC_ACQ_REL);
    }
}

static void inotify_state_free(inotify_state_t *st)
{
    inotify_watch_t *w = st->watches;
    inotify_entry_t *e = st->head;

    spin_lock(&g_inotify_lock);
    inotify_state_t **link = &g_inotify_states;

    while (*link) {
        if (*link == st) {
            *link = st->next;
            break;
        }

        link = &(*link)->next;
    }

    spin_unlock(&g_inotify_lock);

    while (w) {
        inotify_watch_t *next = w->next;
        vfs_node_t *n = w->node;
        kfree(w);
        vfs_node_unref_internal(n);
        w = next;
    }

    while (e) {
        inotify_entry_t *next = e->next;
        kfree(e);
        e = next;
    }

    st->magic = 0;
    kfree(st);
}

void inotify_state_unref(void *state)
{
    inotify_state_t *st = (inotify_state_t *) state;

    if (st && __atomic_sub_fetch(&st->refcnt, 1, __ATOMIC_ACQ_REL) == 0) {
        inotify_state_free(st);
    }
}

int fd_inotify_init(int flags)
{
    if (flags & ~(O_NONBLOCK | O_CLOEXEC)) {
        return -(int) EINVAL;
    }

    inotify_state_t *st = (inotify_state_t *) kcalloc(1, sizeof(*st));

    if (!st) {
        return -(int) ENOMEM;
    }

    st->magic = INOTIFY_MAGIC;
    st->refcnt = 1;
    st->lock.lock = 0;
    st->next_wd = 1;

    int fd = vfs_fd_alloc_from(0);

    if (fd < 0) {
        kfree(st);
        return -(int) EMFILE;
    }

    vfs_file_t *f = vfs_file_alloc();

    if (!f) {
        vfs_fd_clear(fd);
        kfree(st);
        return -(int) ENOMEM;
    }

    f->ifd = st;
    f->flags = O_RDONLY;

    if (flags & O_NONBLOCK) {
        f->flags |= O_NONBLOCK;
    }

    if (flags & O_CLOEXEC) {
        f->cloexec = 1;
    }

    vfs_fd_install(fd, f);

    spin_lock(&g_inotify_lock);
    st->next = g_inotify_states;
    g_inotify_states = st;
    spin_unlock(&g_inotify_lock);

    return fd;
}

int fd_inotify_add_watch(int fd, const char *path, uint32_t mask)
{
    vfs_file_t *f = fd_get_file(fd);

    if (!f || !f->ifd) {
        return -(int) EINVAL;
    }

    char kpath[512];

    if (!vfs_copy_user_path(path, kpath)) {
        return -(int) EFAULT;
    }

    vfs_node_t *n = (mask & IN_DONT_FOLLOW) ? vfs_lookup_nofollow(kpath) : vfs_lookup(kpath);

    if (!n) {
        return -(int) ENOENT;
    }

    if ((mask & IN_ONLYDIR) && n->type != VFS_TYPE_DIR) {
        vfs_node_unref_internal(n);
        return -(int) ENOTDIR;
    }

    inotify_state_t *st = f->ifd;

    spin_lock(&st->lock);

    for (inotify_watch_t *w = st->watches; w; w = w->next) {
        if (w->node != n) {
            continue;
        }

        if (mask & IN_MASK_CREATE) {
            spin_unlock(&st->lock);
            vfs_node_unref_internal(n);
            return -(int) EEXIST;
        }

        w->mask = (mask & IN_MASK_ADD) ? (w->mask | mask) : mask;
        int wd = w->wd;
        spin_unlock(&st->lock);
        vfs_node_unref_internal(n);
        return wd;
    }

    if (st->next_wd <= 0) {
        st->next_wd = 1;
    }

    int wd = st->next_wd++;

    if (st->next_wd <= 0) {
        st->next_wd = 1;
    }

    inotify_watch_t *w = (inotify_watch_t *) kcalloc(1, sizeof(*w));

    if (!w) {
        spin_unlock(&st->lock);
        vfs_node_unref_internal(n);
        return -(int) ENOMEM;
    }

    w->wd = wd;
    w->mask = mask;
    w->node = n;
    w->next = st->watches;
    st->watches = w;

    spin_unlock(&st->lock);
    return wd;
}

int fd_inotify_rm_watch(int fd, int wd)
{
    vfs_file_t *f = fd_get_file(fd);

    if (!f || !f->ifd) {
        return -(int) EINVAL;
    }

    inotify_state_t *st = f->ifd;

    spin_lock(&st->lock);
    inotify_watch_t **link = &st->watches;

    while (*link) {
        if ((*link)->wd == wd) {
            inotify_remove_watch(st, link);
            spin_unlock(&st->lock);
            return 0;
        }

        link = &(*link)->next;
    }

    spin_unlock(&st->lock);
    return -(int) EINVAL;
}

static bool inotify_ready(void *arg)
{
    inotify_state_t *st = (inotify_state_t *) arg;
    bool ready;

    spin_lock(&st->lock);
    ready = st->head != NULL || st->overflow;
    spin_unlock(&st->lock);
    return ready;
}

bool inotify_pollin(void *state)
{
    inotify_state_t *st = (inotify_state_t *) state;

    if (!st) {
        return false;
    }

    return inotify_ready(st);
}

int64_t inotify_read(void *state, char *buf, uint64_t len, int flags)
{
    inotify_state_t *st = (inotify_state_t *) state;

    if (!st) {
        return -(int64_t) EBADF;
    }

    for (;;) {
        spin_lock(&st->lock);

        if (st->overflow) {
            if (len < sizeof(struct inotify_event)) {
                spin_unlock(&st->lock);
                return -(int64_t) EINVAL;
            }

            struct inotify_event ev = { -1, IN_Q_OVERFLOW, 0, 0 };
            memcpy(buf, &ev, sizeof(ev));
            st->overflow = 0;
            spin_unlock(&st->lock);
            return (int64_t) sizeof(ev);
        }

        if (st->head) {
            uint64_t done = 0;

            while (st->head && done + sizeof(struct inotify_event) + st->head->len <= len) {
                inotify_entry_t *e = st->head;
                struct inotify_event ev = { e->wd, e->mask, e->cookie, e->len };

                memcpy(buf + done, &ev, sizeof(ev));
                done += sizeof(ev);

                if (e->len) {
                    memcpy(buf + done, e->name, e->len);
                    done += e->len;
                }

                st->head = e->next;
                st->count--;

                if (!st->head) {
                    st->tail = NULL;
                }

                kfree(e);
            }

            if (done) {
                spin_unlock(&st->lock);
                return (int64_t) done;
            }

            spin_unlock(&st->lock);
            return -(int64_t) EINVAL;
        }

        spin_unlock(&st->lock);

        if (flags & O_NONBLOCK) {
            return -(int64_t) EAGAIN;
        }

        proc_t *p = g_current_proc;

        if (p && proc_blocking_sig_mask(p)) {
            return -(int64_t) EINTR;
        }

        void *object = st;
        poll_wait_once(UINT64_MAX, inotify_ready, st, &object, 1, false);
    }
}

uint32_t inotify_next_cookie(void)
{
    uint32_t cookie = __atomic_add_fetch(&g_inotify_cookie, 1, __ATOMIC_RELAXED);

    if (!cookie) {
        cookie = __atomic_add_fetch(&g_inotify_cookie, 1, __ATOMIC_RELAXED);
    }

    return cookie;
}
