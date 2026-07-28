#include "socket.h"
#include "fs/inet_socket.h"
#include "fs/pipe.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

#define EBADF 9
#define EFAULT 14
#define EINVAL 22

#define SO_TYPE 3
#define SO_ERROR 4
#define SO_PASSCRED 16
#define SO_PEERCRED 17
#define SO_DOMAIN 39
#define SOL_SOCKET 1
#define SCM_CREDENTIALS 2
#define SCM_RIGHTS 1
#define MSG_PEEK 0x0002

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct ucred_s {
    int32_t pid;
    uint32_t uid;
    uint32_t gid;
};

static void sun_path_copy(char *out, const char *sun_path) {
    memcpy(out, sun_path, 108);
    out[108] = '\0';
}

static void path_abs(char *out, const char *in) {
    if (!in || in[0] == '/') {
        strncpy(out, in ? in : "", 511);
        out[511] = '\0';
        return;
    }
    proc_t *p = g_current_proc;
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
    size_t cl = strlen(cwd);
    if (cl >= 511) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    memcpy(out, cwd, cl);
    if (out[cl - 1] != '/') out[cl++] = '/';
    strncpy(out + cl, in, 511 - cl);
    out[511] = '\0';
}

static void fill_peer_cred(vfs_file_t *f, struct ucred_s *cr) {
    if (f && f->peer_pid) {
        cr->pid = (int32_t) f->peer_pid;
        cr->uid = f->peer_uid;
        cr->gid = f->peer_gid;
        return;
    }

    proc_t *p = g_current_proc;
    cr->pid = p ? (int32_t) p->pid : 0;
    cr->uid = p ? p->uid : 0;
    cr->gid = p ? p->gid : 0;
}

static uint64_t align8(uint64_t v) { return (v + 7) & ~7ULL; }

int64_t sys_socket_connect(int fd, struct sockaddr_un *addr, uint64_t addrlen) {
    (void) addrlen;
    if (!addr) return -(int64_t) EFAULT;
    if (!uptr_ok(addr, sizeof(*addr))) return -(int64_t) EFAULT;
    vfs_file_t *f = fd_get_file(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->inet)
        return inet_connect(f->inet, (const struct sockaddr_in *) addr, f->flags);
    char sun[109], abs[512];
    sun_path_copy(sun, addr->sun_path);
    const char *path = sun;
    if (sun[0]) {
        path_abs(abs, sun);
        if (abs[0]) path = abs;
    }
    return (int64_t) fd_connect_unix(fd, path);
}

int64_t sys_socket_accept(int fd, struct sockaddr_un *addr, int *addrlen, int flags) {
    (void) addrlen;
    vfs_file_t *f = fd_get_file(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->inet) {
        struct sockaddr_in *sin = (struct sockaddr_in *) addr;
        if (sin && !uptr_ok_w(sin, sizeof(*sin))) return -(int64_t) EFAULT;
        return inet_accept(f->inet, sin, flags);
    }
    char tmp[108] = { 0 };
    int r = fd_accept_unix(fd, tmp, sizeof(tmp), flags);
    if (r >= 0 && addr) {
        if (!uptr_ok_w(addr, sizeof(*addr))) return -(int64_t) EFAULT;
        addr->sun_family = 1;
        strncpy(addr->sun_path, tmp, 107);
    }
    return (int64_t) r;
}

int64_t sys_socket_bind(int fd, struct sockaddr_un *addr, uint64_t addrlen) {
    (void) addrlen;
    if (!addr) return -(int64_t) EFAULT;
    if (!uptr_ok(addr, sizeof(*addr))) return -(int64_t) EFAULT;
    vfs_file_t *f = fd_get_file(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->inet) return inet_bind(f->inet, (const struct sockaddr_in *) addr);
    char sun[109], abs[512];
    sun_path_copy(sun, addr->sun_path);
    const char *path = sun;
    if (sun[0]) {
        path_abs(abs, sun);
        if (abs[0]) path = abs;
    }
    return (int64_t) fd_bind_unix(fd, path);
}

int64_t sys_socket_getsockname(int fd, struct sockaddr_un *addr, int *addrlen) {
    if (!addr || !addrlen) return -(int64_t) EFAULT;
    if (!uptr_ok_w(addr, sizeof(*addr)) || !uptr_ok_w(addrlen, sizeof(*addrlen)))
        return -(int64_t) EFAULT;
    vfs_file_t *f = fd_get_file(fd);
    if (!f) return -(int64_t) EBADF;
    addr->sun_family = 1;
    addr->sun_path[0] = '\0';
    if (f->node && f->node->type == VFS_TYPE_SOCK) {
        typedef struct {
            int state;
            char path[108];
        } usock_peek_t;
        usock_peek_t *s = (usock_peek_t *) f->node->data;
        if (s && s->path[0]) strncpy(addr->sun_path, s->path, 107);
    }
    *addrlen = (int) sizeof(*addr);
    return 0;
}

int64_t sys_socket_getpeername(int fd, struct sockaddr_un *addr, int *addrlen) {
    (void) fd;
    if (addr) {
        if (!uptr_ok_w(addr, sizeof(*addr))) return -(int64_t) EFAULT;
        addr->sun_family = 1;
        addr->sun_path[0] = '\0';
    }
    if (addrlen) {
        if (!uptr_ok_w(addrlen, sizeof(*addrlen))) return -(int64_t) EFAULT;
        *addrlen = (int) sizeof(*addr);
    }
    return 0;
}

int64_t sys_socket_setsockopt(int fd, int level, int opt, void *val, int optlen) {
    (void) optlen;
    if (level == SOL_SOCKET && opt == SO_PASSCRED) {
        vfs_file_t *sf = fd_get_file(fd);
        if (!sf) return -(int64_t) EBADF;
        if (val && !uptr_ok(val, sizeof(int))) return -(int64_t) EFAULT;
        sf->passcred = val ? (*(int *) val != 0) : 0;
        return 0;
    }
    return 0;
}

int64_t sys_socket_getsockopt(int fd, int level, int opt, void *val, int *optlen) {
    vfs_file_t *f = fd_get_file(fd);
    if (!f) return -(int64_t) EBADF;
    if (!val || !optlen) return -(int64_t) EFAULT;
    if (!uptr_ok_w(optlen, sizeof(*optlen))) return -(int64_t) EFAULT;
    if (level != SOL_SOCKET) return -(int64_t) EINVAL;
    switch (opt) {
    case SO_TYPE:
        if (*optlen < (int) sizeof(int) || !uptr_ok_w(val, sizeof(int))) return -(int64_t) EINVAL;
        *(int *) val = f->inet ? inet_get_type(f->inet) : 1;
        *optlen = sizeof(int);
        return 0;
    case SO_ERROR:
        if (*optlen < (int) sizeof(int) || !uptr_ok_w(val, sizeof(int))) return -(int64_t) EINVAL;
        *(int *) val = f->inet ? inet_get_error(f->inet) : 0;
        *optlen = sizeof(int);
        return 0;
    case SO_PEERCRED:
        if (*optlen < (int) sizeof(struct ucred_s) || !uptr_ok_w(val, sizeof(struct ucred_s)))
            return -(int64_t) EINVAL;
        fill_peer_cred(f, (struct ucred_s *) val);
        *optlen = sizeof(struct ucred_s);
        return 0;
    case SO_DOMAIN:
        if (*optlen < (int) sizeof(int) || !uptr_ok_w(val, sizeof(int))) return -(int64_t) EINVAL;
        *(int *) val = f->inet ? 2 : 1; /* AF_INET=2, AF_UNIX=1 */
        *optlen = sizeof(int);
        return 0;
    }
    return -(int64_t) EINVAL;
}

int64_t sys_socket_sendmsg(int fd, const void *mhdr, int flags) {
    (void) flags;
    if (!mhdr) return -(int64_t) EFAULT;
    if (!uptr_ok(mhdr, 56)) return -(int64_t) EFAULT;
    const uint64_t *m = (const uint64_t *) mhdr;
    const struct iovec *iov = (const struct iovec *) m[2];
    int iovlen = (int) m[3];
    if (iovlen < 0 || (iovlen && !uptr_ok(iov, (uint64_t) iovlen * sizeof(*iov))))
        return -(int64_t) EFAULT;
    int64_t total = 0;
    for (int i = 0; i < iovlen; i++) {
        int64_t r = fd_write(fd, (const void *) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t) r < iov[i].iov_len) break;
    }

    const void *ctrl = (const void *) m[4];
    uint64_t ctrl_len = m[5];
    if (ctrl && ctrl_len >= 16 && uptr_ok(ctrl, ctrl_len)) {
        uint64_t cmsg_len = *(const uint64_t *) ctrl;
        int32_t cmsg_lvl = *(const int32_t *) ((const uint8_t *) ctrl + 8);
        int32_t cmsg_typ = *(const int32_t *) ((const uint8_t *) ctrl + 12);
        if (cmsg_lvl == SOL_SOCKET && cmsg_typ == SCM_RIGHTS && cmsg_len >= 16 &&
            cmsg_len <= ctrl_len) {
            const int *fdarr = (const int *) ((const uint8_t *) ctrl + 16);
            int nfds = (int) ((cmsg_len - 16) / sizeof(int));
            vfs_file_t *sf = fd_get_file(fd);
            pipe_t *tx = sf ? (sf->wpipe ? sf->wpipe : sf->pipe) : NULL;
            if (tx && nfds > 0) {
                void *nodes[PIPE_ANC_MAXFDS];
                int n = nfds < PIPE_ANC_MAXFDS ? nfds : PIPE_ANC_MAXFDS;
                for (int i = 0; i < n; i++) nodes[i] = fd_get_node(fdarr[i]);
                pipe_anc_send(tx, nodes, n);
            }
        }
    }
    return total;
}

int64_t sys_socket_recvmsg(int fd, void *mhdr, int flags) {
    if (!mhdr) return -(int64_t) EFAULT;
    if (!uptr_ok_w(mhdr, 56)) return -(int64_t) EFAULT;
    uint64_t *m = (uint64_t *) mhdr;
    const struct iovec *iov = (const struct iovec *) m[2];
    int iovlen = (int) m[3];
    if (iovlen < 0 || (iovlen && !uptr_ok(iov, (uint64_t) iovlen * sizeof(*iov))))
        return -(int64_t) EFAULT; // yes im fixed this, 15.06.2026 im so tired
    {
        vfs_file_t *isf = fd_get_file(fd);
        if (isf && isf->inet && iovlen > 0 && !(flags & MSG_PEEK)) {
            if (!uptr_ok_w((void *) iov[0].iov_base, iov[0].iov_len)) return -(int64_t) EFAULT;
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            int64_t r = inet_recvfrom(isf->inet, (void *) iov[0].iov_base, iov[0].iov_len, &src,
                                      isf->flags);
            if (r < 0) return r;
            void *name = (void *) m[0];
            uint32_t namelen = ((uint32_t *) mhdr)[2]; /* msg_namelen at offset 8 */
            if (name && namelen >= sizeof(src) && uptr_ok_w(name, sizeof(src))) {
                memcpy(name, &src, sizeof(src));
                ((uint32_t *) mhdr)[2] = (uint32_t) sizeof(src);
            } else {
                ((uint32_t *) mhdr)[2] = 0;
            }
            m[5] = 0;                    /* msg_controllen */
            ((uint32_t *) mhdr)[12] = 0; /* msg_flags */
            return r;
        }
    }

    int64_t total = 0;
    uint64_t peek_skip = 0;
    for (int i = 0; i < iovlen; i++) {
        int64_t r = (flags & MSG_PEEK) ?
                        fd_peek(fd, (void *) iov[i].iov_base, iov[i].iov_len, peek_skip) :
                        fd_read(fd, (void *) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if (flags & MSG_PEEK) peek_skip += (uint64_t) r;
        if ((uint64_t) r < iov[i].iov_len) break;
    }

    void *control = (void *) m[4];
    uint64_t control_len = m[5];
    uint64_t cmsg_hdr_len = 16;
    vfs_file_t *sf = fd_get_file(fd);

    if (sf && control && control_len >= cmsg_hdr_len) {
        void *nodes[PIPE_ANC_MAXFDS];
        pipe_t *rx = sf->pipe;
        int nfds = rx ? pipe_anc_recv(rx, nodes, PIPE_ANC_MAXFDS) : 0;
        if (nfds > 0) {
            uint64_t rights_data = (uint64_t) nfds * sizeof(int);
            uint64_t rights_len = cmsg_hdr_len + rights_data;
            uint64_t rights_sp = align8(rights_len);
            if (control_len >= rights_sp && uptr_ok_w(control, rights_sp)) {
                *(uint64_t *) control = rights_len;
                *(int32_t *) ((uint8_t *) control + 8) = SOL_SOCKET;
                *(int32_t *) ((uint8_t *) control + 12) = SCM_RIGHTS;
                int *fdarr = (int *) ((uint8_t *) control + cmsg_hdr_len);
                for (int i = 0; i < nfds; i++) {
                    vfs_node_t *nd = (vfs_node_t *) nodes[i];
                    fdarr[i] = nd ? fd_open_node(nd, O_RDWR) : -1;
                }
                m[5] = rights_sp;
                ((uint32_t *) mhdr)[12] = 0;
                return total;
            }
        }
    }

    uint64_t cred_len = sizeof(struct ucred_s);
    uint64_t cmsg_len = cmsg_hdr_len + cred_len;
    uint64_t cmsg_space = align8(cmsg_len);
    if (sf && sf->passcred && control && control_len >= cmsg_space) {
        if (!uptr_ok_w(control, cmsg_space)) return -(int64_t) EFAULT;
        uint64_t *cmsg_len_p = (uint64_t *) control;
        int32_t *cmsg_meta = (int32_t *) ((uint8_t *) control + 8);
        struct ucred_s *cr = (struct ucred_s *) ((uint8_t *) control + cmsg_hdr_len);
        *cmsg_len_p = cmsg_len;
        cmsg_meta[0] = SOL_SOCKET;
        cmsg_meta[1] = SCM_CREDENTIALS;
        fill_peer_cred(sf, cr);
        m[5] = cmsg_space;
    } else {
        m[5] = 0;
    }
    ((uint32_t *) mhdr)[12] = 0;
    return total;
}
