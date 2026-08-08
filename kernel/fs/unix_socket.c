#include "unix_socket.h"
#include "arch/x86_64/spinlock.h"
#include "fs/pipe.h"
#include "fs/vfs_internal.h"
#include "inet_socket.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "syscall/poll.h"

#define EACCES 13
#define EADDRINUSE 98
#define EAGAIN 11
#define EBADF 9
#define ECONNREFUSED 111
#define EEXIST 17
#define EINVAL 22
#define EMFILE 24
#define ENOENT 2
#define ENOMEM 12
#define EOPNOTSUPP 95

#define SOCK_UNBOUND 0
#define SOCK_BOUND 1
#define SOCK_LISTENING 2

#define MAX_ABSTRACT_SOCKS 16

typedef struct unix_conn {
    pipe_t *cli_rx;
    pipe_t *srv_rx;
    uint32_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;
    struct unix_conn *next;
} unix_conn_t;

typedef struct {
    int state;
    char path[108];
    unix_conn_t *backlog;
    unix_conn_t *backlog_tail;
    proc_t *accept_waiter;
    spinlock_t lock;
} unix_sock_t;

static struct {
    char name[107];
    vfs_node_t *node;
    uint32_t jail_id;
} g_abstract_socks[MAX_ABSTRACT_SOCKS];

static uint32_t cur_jail(void) { return g_current_proc ? g_current_proc->jail_id : JAIL_HOST; }

static bool ipc_isolated(void) {
    jail_t *j = jail_find(cur_jail());
    return j && (j->flags & JAILF_IPC);
}

int fd_socket(int domain, int type, int proto) {
    (void) proto;
    if (domain == 2) return fd_inet_socket(type, proto); /* AF_INET */
    if (domain != 1) return -(int) EINVAL;
    if ((type & 0xf) != 1) return -(int) EOPNOTSUPP; /* only SOCK_STREAM supported on AF_UNIX */
    unix_sock_t *s = (unix_sock_t *) kcalloc(1, sizeof(unix_sock_t));
    if (!s) return -(int) ENOMEM;
    vfs_node_t *n = vfs_node_alloc_internal("", VFS_TYPE_SOCK, S_IFSOCK | 0666);
    if (!n) {
        kfree(s);
        return -(int) ENOMEM;
    }
    n->data = (uint8_t *) s;
    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        kfree(s);
        kfree(n);
        return -(int) EMFILE;
    }
    vfs_file_t *f = vfs_file_alloc();
    if (!f) {
        vfs_fd_clear(fd);
        kfree(s);
        kfree(n);
        return -(int) ENOMEM;
    }
    f->node = n;
    vfs_node_ref_internal(n);
    f->flags = O_RDWR | (type & O_NONBLOCK);
    f->cloexec = (type & O_CLOEXEC) ? 1 : 0;
    vfs_fd_install(fd, f);
    return fd;
}

int fd_bind_unix(int fd, const char *path) {
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK) return -(int) EBADF;
    unix_sock_t *s = (unix_sock_t *) f->node->data;
    if (s->state != SOCK_UNBOUND) return -(int) EINVAL;

    if (path[0] == '\0') {
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++) {
            if (!g_abstract_socks[i].node) {
                strncpy(g_abstract_socks[i].name, path + 1, 106);
                g_abstract_socks[i].name[106] = '\0';
                g_abstract_socks[i].node = f->node;
                g_abstract_socks[i].jail_id = cur_jail();
                s->path[0] = '\0';
                strncpy(s->path + 1, path + 1, 106);
                s->state = SOCK_BOUND;
                return 0;
            }
        }
        return -(int) EADDRINUSE;
    }

    char ppath[512];
    strncpy(ppath, path, sizeof(ppath) - 1);
    ppath[sizeof(ppath) - 1] = '\0';
    char *slash = NULL;
    for (char *p = ppath + strlen(ppath); p >= ppath; p--)
        if (*p == '/') {
            slash = p;
            break;
        }
    if (!slash) return -(int) EINVAL;
    const char *leaf = slash + 1;
    if (!*leaf) return -(int) EINVAL;
    *slash = '\0';
    vfs_node_t *parent = vfs_lookup(ppath[0] ? ppath : "/");
    if (!parent || parent->type != VFS_TYPE_DIR) {
        vfs_node_unref_internal(parent);
        return -(int) ENOENT;
    }
    if (!vfs_may_create_in_internal(parent)) {
        vfs_node_unref_internal(parent);
        return -(int) EACCES;
    }
    if (vfs_dir_find_internal(parent, leaf)) {
        vfs_node_unref_internal(parent);
        return -(int) EEXIST;
    }
    vfs_node_t *bn = vfs_node_alloc_internal(leaf, VFS_TYPE_SOCK, S_IFSOCK | 0666);
    if (!bn) {
        vfs_node_unref_internal(parent);
        return -(int) ENOMEM;
    }
    bn->data = (uint8_t *) s;
    vfs_dir_insert_internal(parent, bn);
    vfs_node_unref_internal(parent);
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->state = SOCK_BOUND;
    return 0;
}

int fd_listen_unix(int fd, int backlog) {
    (void) backlog;
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK) return -(int) EBADF;
    unix_sock_t *s = (unix_sock_t *) f->node->data;
    if (s->state == SOCK_UNBOUND) return -(int) EINVAL;
    s->state = SOCK_LISTENING;
    return 0;
}

int fd_accept_unix(int fd, char *path_out, int path_max, int flags) {
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK) return -(int) EBADF;
    unix_sock_t *s = (unix_sock_t *) f->node->data;
    if (s->state != SOCK_LISTENING) return -(int) EINVAL;
    if (!s->backlog && (f->flags & O_NONBLOCK)) return -(int) EAGAIN;
    s->accept_waiter = g_current_proc;
    while (!s->backlog) sched_yield_blocking();
    s->accept_waiter = NULL;
    spin_lock(&s->lock);
    unix_conn_t *conn = s->backlog;
    s->backlog = conn->next;
    if (!s->backlog) s->backlog_tail = NULL;
    f->node->sock_backlog--;
    spin_unlock(&s->lock);
    pipe_t *srv_rx = conn->srv_rx;
    pipe_t *cli_rx = conn->cli_rx;
    uint32_t peer_pid = conn->peer_pid;
    uint32_t peer_uid = conn->peer_uid;
    uint32_t peer_gid = conn->peer_gid;
    kfree(conn);
    int nfd = vfs_fd_alloc_from(0);
    if (nfd < 0) {
        vfs_pipe_drop_read(srv_rx);
        vfs_pipe_drop_write(cli_rx);
        return -(int) EMFILE;
    }
    vfs_file_t *nf = vfs_file_alloc();
    if (!nf) {
        vfs_fd_clear(nfd);
        vfs_pipe_drop_read(srv_rx);
        vfs_pipe_drop_write(cli_rx);
        return -(int) ENOMEM;
    }
    nf->pipe = srv_rx;
    nf->wpipe = cli_rx;
    nf->pipe_end = PIPE_END_READ;
    nf->flags = O_RDWR | (flags & O_NONBLOCK);
    nf->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    nf->peer_pid = peer_pid;
    nf->peer_uid = peer_uid;
    nf->peer_gid = peer_gid;
    vfs_fd_install(nfd, nf);
    if (path_out && path_max > 0) path_out[0] = '\0';
    return nfd;
}

int fd_connect_unix(int fd, const char *path) {
    vfs_file_t *f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK) return -(int) EBADF;

    vfs_node_t *sn;
    if (path[0] == '\0') {
        sn = NULL;
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++) {
            if (g_abstract_socks[i].node && strncmp(g_abstract_socks[i].name, path + 1, 106) == 0) {
                if (ipc_isolated() && g_abstract_socks[i].jail_id != cur_jail())
                    continue; /* abstract name in another jail is invisible */
                sn = g_abstract_socks[i].node;
                break;
            }
        }
    } else {
        sn = vfs_lookup(path);
    }
    if (!sn || sn->type != VFS_TYPE_SOCK) {
        vfs_node_unref_internal(sn);
        return -(int) ECONNREFUSED;
    }
    unix_sock_t *srv = (unix_sock_t *) sn->data;
    if (!srv || srv->state != SOCK_LISTENING) {
        vfs_node_unref_internal(sn);
        return -(int) ECONNREFUSED;
    }
    pipe_t *cli_rx = pipe_alloc();
    pipe_t *srv_rx = pipe_alloc();
    if (!cli_rx || !srv_rx) {
        pipe_free(cli_rx);
        pipe_free(srv_rx);
        return -(int) ENOMEM;
    }
    pipe_ref_read(cli_rx);
    pipe_ref_write(cli_rx);
    pipe_ref_read(srv_rx);
    pipe_ref_write(srv_rx);
    unix_conn_t *conn = (unix_conn_t *) kcalloc(1, sizeof(unix_conn_t));
    if (!conn) {
        pipe_free(cli_rx);
        pipe_free(srv_rx);
        return -(int) ENOMEM;
    }
    conn->cli_rx = cli_rx;
    conn->srv_rx = srv_rx;
    conn->peer_pid = g_current_proc ? g_current_proc->pid : 0;
    conn->peer_uid = g_current_proc ? g_current_proc->uid : 0;
    conn->peer_gid = g_current_proc ? g_current_proc->gid : 0;
    spin_lock(&srv->lock);
    if (!srv->backlog) {
        srv->backlog = conn;
    } else {
        srv->backlog_tail->next = conn;
    }
    srv->backlog_tail = conn;
    sn->sock_backlog++;
    if (srv->accept_waiter) {
        proc_t *w = srv->accept_waiter;
        if (__sync_bool_compare_and_swap(&w->state, PROC_WAITING, PROC_READY)) proc_set_ready(w);
    }
    spin_unlock(&srv->lock);
    poll_notify_object(sn);
    vfs_node_unref_internal(sn);
    unix_sock_t *cs = (unix_sock_t *) f->node->data;
    kfree(cs);
    vfs_node_mark_deleted_internal(f->node);
    vfs_node_unref_internal(f->node);
    f->node = NULL;
    f->pipe = cli_rx;
    f->wpipe = srv_rx;
    f->pipe_end = PIPE_END_READ;
    return 0;
}

void unix_socket_close(vfs_file_t *f) {
    unix_sock_t *s = (unix_sock_t *) f->node->data;
    if (s) {
        if (s->path[0]) {
            vfs_unlink(s->path);
        } else if (s->path[1]) {
            for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++) {
                if (g_abstract_socks[i].node == f->node) {
                    g_abstract_socks[i].node = NULL;
                    break;
                }
            }
        }
        unix_conn_t *c = s->backlog;
        s->backlog = NULL;
        s->backlog_tail = NULL;
        while (c) {
            unix_conn_t *nx = c->next;
            vfs_pipe_drop_write(c->cli_rx);
            vfs_pipe_drop_read(c->srv_rx);
            kfree(c);
            c = nx;
        }
        kfree(s);
    }
    vfs_node_mark_deleted_internal(f->node);
}
