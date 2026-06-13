#include "unix_socket.h"
#include "fs/pipe.h"
#include "fs/vfs_internal.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"

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

#define SOCK_UNBOUND 0
#define SOCK_BOUND 1
#define SOCK_LISTENING 2

#define MAX_ABSTRACT_SOCKS 16

typedef struct unix_conn {
    pipe_t* cli_rx;
    pipe_t* srv_rx;
    uint32_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;
    struct unix_conn* next;
} unix_conn_t;

typedef struct {
    int state;
    char path[108];
    unix_conn_t* backlog;
    proc_t* accept_waiter;
} unix_sock_t;

static struct {
    char name[107];
    vfs_node_t* node;
} g_abstract_socks[MAX_ABSTRACT_SOCKS];

int fd_socket(int domain, int type, int proto)
{
    (void) proto;
    if (domain != 1 || (type & 0xf) != 1)
        return -(int) EINVAL;
    unix_sock_t* s = (unix_sock_t*) kcalloc(1, sizeof(unix_sock_t));
    if (!s)
        return -(int) ENOMEM;
    vfs_node_t* n = vfs_node_alloc_internal("", VFS_TYPE_SOCK, S_IFSOCK | 0666);
    if (!n) {
        kfree(s);
        return -(int) ENOMEM;
    }
    n->data = (uint8_t*) s;
    int fd = vfs_fd_alloc_from(0);
    if (fd < 0) {
        kfree(s);
        kfree(n);
        return -(int) EMFILE;
    }
    vfs_file_t* f = vfs_file_alloc();
    if (!f) {
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

int fd_bind_unix(int fd, const char* path)
{
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;
    unix_sock_t* s = (unix_sock_t*) f->node->data;
    if (s->state != SOCK_UNBOUND)
        return -(int) EINVAL;

    if (path[0] == '\0') {
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++) {
            if (!g_abstract_socks[i].node) {
                strncpy(g_abstract_socks[i].name, path + 1, 106);
                g_abstract_socks[i].name[106] = '\0';
                g_abstract_socks[i].node = f->node;
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
    char* slash = NULL;
    for (char* p = ppath + strlen(ppath); p >= ppath; p--)
        if (*p == '/') {
            slash = p;
            break;
        }
    if (!slash)
        return -(int) EINVAL;
    const char* leaf = slash + 1;
    if (!*leaf)
        return -(int) EINVAL;
    *slash = '\0';
    vfs_node_t* parent = vfs_lookup(ppath[0] ? ppath : "/");
    if (!parent || parent->type != VFS_TYPE_DIR)
        return -(int) ENOENT;
    if (!vfs_may_create_in_internal(parent))
        return -(int) EACCES;
    if (vfs_dir_find_internal(parent, leaf))
        return -(int) EEXIST;
    vfs_node_t* bn = vfs_node_alloc_internal(leaf, VFS_TYPE_SOCK, S_IFSOCK | 0666);
    if (!bn)
        return -(int) ENOMEM;
    bn->data = (uint8_t*) s;
    vfs_dir_insert_internal(parent, bn);
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->state = SOCK_BOUND;
    return 0;
}

int fd_listen_unix(int fd, int backlog)
{
    (void) backlog;
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;
    unix_sock_t* s = (unix_sock_t*) f->node->data;
    if (s->state == SOCK_UNBOUND)
        return -(int) EINVAL;
    s->state = SOCK_LISTENING;
    return 0;
}

int fd_accept_unix(int fd, char* path_out, int path_max, int flags)
{
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;
    unix_sock_t* s = (unix_sock_t*) f->node->data;
    if (s->state != SOCK_LISTENING)
        return -(int) EINVAL;
    if (!s->backlog && (f->flags & O_NONBLOCK))
        return -(int) EAGAIN;
    s->accept_waiter = g_current_proc;
    while (!s->backlog)
        sched_yield_blocking();
    s->accept_waiter = NULL;
    unix_conn_t* conn = s->backlog;
    s->backlog = conn->next;
    f->node->sock_backlog--;
    pipe_t* srv_rx = conn->srv_rx;
    pipe_t* cli_rx = conn->cli_rx;
    uint32_t peer_pid = conn->peer_pid;
    uint32_t peer_uid = conn->peer_uid;
    uint32_t peer_gid = conn->peer_gid;
    kfree(conn);
    int nfd = vfs_fd_alloc_from(0);
    if (nfd < 0) {
        if (srv_rx->read_refs)
            srv_rx->read_refs--;
        vfs_pipe_maybe_free(srv_rx);
        vfs_pipe_drop_write(cli_rx);
        vfs_pipe_maybe_free(cli_rx);
        return -(int) EMFILE;
    }
    vfs_file_t* nf = vfs_file_alloc();
    if (!nf) {
        if (srv_rx->read_refs)
            srv_rx->read_refs--;
        vfs_pipe_maybe_free(srv_rx);
        vfs_pipe_drop_write(cli_rx);
        vfs_pipe_maybe_free(cli_rx);
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
    if (path_out && path_max > 0)
        path_out[0] = '\0';
    return nfd;
}

int fd_connect_unix(int fd, const char* path)
{
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;

    vfs_node_t* sn;
    if (path[0] == '\0') {
        sn = NULL;
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++) {
            if (g_abstract_socks[i].node && strncmp(g_abstract_socks[i].name, path + 1, 106) == 0) {
                sn = g_abstract_socks[i].node;
                break;
            }
        }
    } else {
        sn = vfs_lookup(path);
    }
    if (!sn || sn->type != VFS_TYPE_SOCK)
        return -(int) ECONNREFUSED;
    unix_sock_t* srv = (unix_sock_t*) sn->data;
    if (!srv || srv->state != SOCK_LISTENING)
        return -(int) ECONNREFUSED;
    pipe_t* cli_rx = pipe_alloc();
    pipe_t* srv_rx = pipe_alloc();
    if (!cli_rx || !srv_rx) {
        pipe_free(cli_rx);
        pipe_free(srv_rx);
        return -(int) ENOMEM;
    }
    cli_rx->read_refs = 1;
    cli_rx->write_refs = 1;
    srv_rx->read_refs = 1;
    srv_rx->write_refs = 1;
    unix_conn_t* conn = (unix_conn_t*) kcalloc(1, sizeof(unix_conn_t));
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
    if (!srv->backlog) {
        srv->backlog = conn;
    } else {
        unix_conn_t* tail = srv->backlog;
        while (tail->next)
            tail = tail->next;
        tail->next = conn;
    }
    sn->sock_backlog++;
    if (srv->accept_waiter && srv->accept_waiter->state == PROC_WAITING)
        srv->accept_waiter->state = PROC_READY;
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state == PROC_WAITING)
            g_proctable[i].state = PROC_READY;
    unix_sock_t* cs = (unix_sock_t*) f->node->data;
    kfree(cs);
    vfs_node_mark_deleted_internal(f->node);
    vfs_node_unref_internal(f->node);
    f->node = NULL;
    f->pipe = cli_rx;
    f->wpipe = srv_rx;
    f->pipe_end = PIPE_END_READ;
    return 0;
}

void unix_socket_close(vfs_file_t* f)
{
    unix_sock_t* s = (unix_sock_t*) f->node->data;
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
        unix_conn_t* c = s->backlog;
        while (c) {
            unix_conn_t* nx = c->next;
            vfs_pipe_drop_write(c->cli_rx);
            vfs_pipe_maybe_free(c->cli_rx);
            if (c->srv_rx->read_refs)
                c->srv_rx->read_refs--;
            vfs_pipe_maybe_free(c->srv_rx);
            kfree(c);
            c = nx;
        }
        kfree(s);
    }
    vfs_node_mark_deleted_internal(f->node);
}
