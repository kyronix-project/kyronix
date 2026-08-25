#include "file.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22
#define ENOENT 2
#define ENOMEM 12

#define STATX_BASIC_STATS 0x7ffU
#define FILE_COPY_CHUNK (64U * 1024U)

static int64_t copy_fd_data(int outfd, uint64_t out_off, bool positional_out, int infd,
                            uint64_t in_off, uint64_t count) {
    if (in_off > INT64_MAX || (positional_out && out_off > INT64_MAX))
        return -(int64_t) EINVAL;

    uint8_t *buf = (uint8_t *) kmalloc(FILE_COPY_CHUNK);
    if (!buf) return -(int64_t) ENOMEM;

    uint64_t total = 0;
    int64_t result = 0;
    if (count > INT64_MAX) count = INT64_MAX;
    while (total < count) {
        uint64_t chunk = count - total;
        if (chunk > FILE_COPY_CHUNK) chunk = FILE_COPY_CHUNK;
        int64_t got = fd_pread_kbuf(infd, buf, chunk, in_off + total);
        if (got <= 0) {
            result = (got < 0 && total == 0) ? got : (int64_t) total;
            break;
        }
        int64_t wrote = positional_out
                            ? fd_pwrite_kbuf(outfd, buf, (uint64_t) got, out_off + total)
                            : fd_write_kbuf(outfd, buf, (uint64_t) got);
        if (wrote <= 0) {
            result = (wrote < 0 && total == 0) ? wrote : (int64_t) total;
            break;
        }
        total += (uint64_t) wrote;
        result = (int64_t) total;
        if (wrote < got) break;
    }
    kfree(buf);
    return result;
}

int64_t sys_readv(int fd, const struct iovec *iov, int n) {
    if (n < 0 || n > 1024) return -(int64_t) EINVAL;
    if (n && (!iov || !uptr_ok(iov, (uint64_t) n * sizeof(*iov)))) return -(int64_t) EFAULT;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_read(fd, (void *) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            if (!total) total = r;
            break;
        }
        total += r;
        if ((uint64_t) r < iov[i].iov_len) break;
    }
    return total;
}

int64_t sys_writev(int fd, const void *iov_ptr, int n) {
    if (n < 0 || n > 1024) return -(int64_t) EINVAL;
    if (n && (!iov_ptr || !uptr_ok(iov_ptr, (uint64_t) n * sizeof(struct iovec))))
        return -(int64_t) EFAULT;
    const struct iovec *iov = (const struct iovec *) iov_ptr;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_write(fd, (const void *) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            if (!total) total = r;
            break;
        }
        total += r;
        if ((uint64_t) r < iov[i].iov_len) break;
    }
    return total;
}

int64_t sys_sendfile(int outfd, int infd, uint64_t *offp, uint64_t count) {
    if (offp && !uptr_ok_w(offp, sizeof(*offp))) return -(int64_t) EFAULT;
    vfs_file_t *inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    uint64_t off = offp ? *offp : inf->pos;
    int64_t w = copy_fd_data(outfd, 0, false, infd, off, count);
    if (w > 0) {
        if (offp)
            *offp = off + (uint64_t) w;
        else
            inf->pos = off + (uint64_t) w;
    }
    return w;
}

int64_t sys_preadv(int fd, const struct iovec *iov, int n, uint64_t off) {
    if (n < 0 || n > 1024) return -(int64_t) EINVAL;
    if (n && (!iov || !uptr_ok(iov, (uint64_t) n * sizeof(*iov)))) return -(int64_t) EFAULT;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_pread(fd, (void *) iov[i].iov_base, iov[i].iov_len, off + (uint64_t) total);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t) r < iov[i].iov_len) break;
    }
    return total;
}

int64_t sys_pwritev(int fd, const struct iovec *iov, int n, uint64_t off) {
    if (n < 0 || n > 1024) return -(int64_t) EINVAL;
    if (n && (!iov || !uptr_ok(iov, (uint64_t) n * sizeof(*iov)))) return -(int64_t) EFAULT;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r =
            fd_pwrite(fd, (const void *) iov[i].iov_base, iov[i].iov_len, off + (uint64_t) total);
        if (r < 0) return total ? total : r;
        total += r;
    }
    return total;
}

static uint32_t g_memfd_seq;

#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

int64_t sys_memfd_create(const char *name, uint32_t flags) {
    if (flags & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) return -(int64_t) EINVAL;
    char kname[512];
    if (name) {
        if (!vfs_copy_user_path(name, kname)) return -(int64_t) EFAULT;
    } else {
        snprintf(kname, sizeof(kname), "anon%u", ++g_memfd_seq);
    }
    return fd_memfd_open(kname, (flags & MFD_CLOEXEC) != 0);
}

int64_t sys_copy_file_range(int infd, uint64_t *off_in, int outfd, uint64_t *off_out, uint64_t len,
                            uint32_t flags) {
    (void) flags;
    if (off_in && !uptr_ok_w(off_in, sizeof(*off_in))) return -(int64_t) EFAULT;
    if (off_out && !uptr_ok_w(off_out, sizeof(*off_out))) return -(int64_t) EFAULT;
    vfs_file_t *inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    uint64_t rin = off_in ? *off_in : inf->pos;
    uint64_t rout = off_out ? *off_out : (fd_get_file(outfd) ? fd_get_file(outfd)->pos : 0);
    int64_t w = copy_fd_data(outfd, rout, true, infd, rin, len);
    if (w > 0) {
        if (off_in)
            *off_in = rin + (uint64_t) w;
        else
            inf->pos = rin + (uint64_t) w;
        if (off_out)
            *off_out = rout + (uint64_t) w;
        else if (fd_get_file(outfd))
            fd_get_file(outfd)->pos = rout + (uint64_t) w;
    }
    return w;
}

int64_t sys_statx(int dirfd, const char *path, int flags, uint32_t mask, struct statx *sx) {
    (void) mask;
    if (!sx) return -(int64_t) EFAULT;
    if (!uptr_ok_w(sx, sizeof(*sx))) return -(int64_t) EFAULT;
    char kpath[512];
    if (path && !vfs_copy_user_path(path, kpath)) return -(int64_t) EFAULT;
    vfs_node_t *n = NULL;
    if (!path || kpath[0] == '\0') {
        n = fd_get_node(dirfd);
    } else {
        char abs[512];
        int ar = at_resolve(dirfd, path, abs, sizeof(abs));
        if (ar < 0) return ar;
        n = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(abs) : vfs_lookup(abs);
    }
    if (!n) return -(int64_t) ENOENT;
    memset(sx, 0, sizeof(*sx));
    sx->stx_mask = STATX_BASIC_STATS;
    sx->stx_blksize = 4096;
    sx->stx_nlink = 1;
    sx->stx_uid = n->uid;
    sx->stx_gid = n->gid;
    sx->stx_mode = (uint16_t) n->mode;
    sx->stx_ino = n->ino;
    sx->stx_size = n->size;
    sx->stx_blocks = (n->size + 511) / 512;
    sx->stx_dev_major = 1;
    sx->stx_rdev_major = (n->rdev >> 8) & 0xfffU;
    sx->stx_rdev_minor = n->rdev & 0xffU;
    vfs_node_unref_internal(n);
    return 0;
}
