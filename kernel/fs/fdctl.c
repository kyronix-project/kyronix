#include "fdctl.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/string.h"
#include "syscall/syscall.h"

#define EBADF 9
#define EFAULT 14
#define EINVAL 22
#define EMFILE 24
#define ENOMEM 12
#define ENOTTY 25

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TIOCGWINSZ 0x5413
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define FIONBIO 0x5421
#define FIOCLEX 0x5451

struct winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct termios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_cc[19];
};

int fd_ioctl(int fd, uint64_t req, uint64_t arg)
{
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f)
        return -(int) EBADF;
    if (f->pipe)
        return -(int) ENOTTY;

    if (f->node && f->node->type == VFS_TYPE_CHR && f->node->chr_ioctl)
        return (int) f->node->chr_ioctl(f->node, req, arg);
    // YEYEYEYEYEYEY IM FIXED THIS FUCKING SHIT
    switch ((uint32_t) req) {
    case TIOCGWINSZ: {
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws)
            return -(int) EINVAL;
        if (!uptr_ok_w(ws, sizeof(*ws)))
            return -(int) EFAULT;
        ws->ws_row = 25;
        ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TCGETS: {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (!t)
            return -(int) EINVAL;
        if (!uptr_ok_w(t, sizeof(*t)))
            return -(int) EFAULT;
        memset(t, 0, sizeof(*t));
        t->c_iflag = 0x500;
        t->c_oflag = 0x5;
        t->c_cflag = 0xBF;
        t->c_lflag = tty_get_lflag();
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case 0x5404: {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (t) {
            if (!uptr_ok(t, sizeof(*t)))
                return -(int) EFAULT;
            tty_set_lflag(t->c_lflag);
        }
        return 0;
    }
    case 0x5405:
    case 0x5406:
    case 0x540B:
    case 0x5422:
    case 0x541B: {
        int* n = (int*) (uintptr_t) arg;
        if (!n)
            return -(int) EINVAL;
        if (!uptr_ok_w(n, sizeof(*n)))
            return -(int) EFAULT;
        *n = (int) (f->pipe ? f->pipe->count : 0);
        return 0;
    }
    case FIONBIO:
    case FIOCLEX:
        return 0;
    case TIOCGPGRP: {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid) {
            if (!uptr_ok_w(pgid, sizeof(*pgid)))
                return -(int) EFAULT;
            *pgid = tty_get_fg_pgid();
        }
        return 0;
    }
    case TIOCSPGRP: {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid) {
            if (!uptr_ok(pgid, sizeof(*pgid)))
                return -(int) EFAULT;
            tty_set_fg_pgid(*pgid);
        }
        return 0;
    }
    default:
        return -(int) EINVAL;
    }
}

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define F_GETLK 5
#define F_SETLK 6
#define F_SETLKW 7
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC 1

int fd_fcntl(int fd, int cmd, uint64_t arg)
{
    vfs_file_t* f = vfs_fd_get(fd);
    if (!f)
        return -(int) EBADF;
    switch (cmd) {
    case F_GETFD:
        return f->cloexec ? FD_CLOEXEC : 0;
    case F_SETFD:
        f->cloexec = (arg & FD_CLOEXEC) ? 1 : 0;
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:
        f->flags = (int) arg;
        return 0;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC: {
        int newfd = vfs_fd_alloc_from((int) arg);
        if (newfd < 0)
            return -(int) EMFILE;
        vfs_file_t* nf = vfs_file_alloc();
        if (!nf)
            return -(int) ENOMEM;
        *nf = *f;
        nf->cloexec = (cmd == F_DUPFD_CLOEXEC) ? 1 : 0;
        vfs_file_addref(nf);
        vfs_fd_install(newfd, nf);
        return newfd;
    }
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        return 0;
    default:
        return -(int) EINVAL;
    }
}

int fd_dup(int oldfd)
{
    return fd_fcntl(oldfd, F_DUPFD, 0);
}

int fd_dup2(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return fd_valid(oldfd) ? oldfd : -(int) EBADF;
    vfs_file_t* f = vfs_fd_get(oldfd);
    if (!f)
        return -(int) EBADF;
    if (newfd < 0 || newfd >= VFS_FD_MAX)
        return -(int) EBADF;
    vfs_file_t* old = vfs_fd_get(newfd);
    if (old) {
        vfs_file_close(old);
        vfs_fd_clear(newfd);
    }
    vfs_file_t* nf = vfs_file_alloc();
    if (!nf)
        return -(int) ENOMEM;
    *nf = *f;
    nf->cloexec = 0;
    vfs_file_addref(nf);
    vfs_fd_install(newfd, nf);
    return newfd;
}

int fd_dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd)
        return -(int) EINVAL;
    int r = fd_dup2(oldfd, newfd);
    if (r >= 0) {
        vfs_file_t* nf = vfs_fd_get(r);
        if (nf)
            nf->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    }
    return r;
}
