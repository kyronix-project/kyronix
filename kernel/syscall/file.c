#include "file.h"
#include "fs/vfs.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "syscall/syscall.h"

#define EFAULT 14
#define EINVAL 22
#define ENOENT 2

#define STATX_BASIC_STATS 0x7ffU

int64_t sys_readv(int fd, const struct iovec* iov, int n)
{
    if (n < 0 || n > 1024)
        return -(int64_t) EINVAL;
    if (n && (!iov || !uptr_ok(iov, (uint64_t) n * sizeof(*iov))))
        return -(int64_t) EFAULT;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_read(fd, (void*) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            if (!total)
                total = r;
            break;
        }
        total += r;
        if ((uint64_t) r < iov[i].iov_len)
            break;
    }
    return total;
}

int64_t sys_writev(int fd, const void* iov_ptr, int n)
{
    if (n < 0 || n > 1024)
        return -(int64_t) EINVAL;
    if (n && (!iov_ptr || !uptr_ok(iov_ptr, (uint64_t) n * sizeof(struct iovec))))
        return -(int64_t) EFAULT;
    const struct iovec* iov = (const struct iovec*) iov_ptr;
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_write(fd, (const void*) iov[i].iov_base, iov[i].iov_len);
        if (r < 0) {
            if (!total)
                total = r;
            break;
        }
        total += r;
        if ((uint64_t) r < iov[i].iov_len)
            break;
    }
    return total;
}

int64_t sys_sendfile(int outfd, int infd, uint64_t* offp, uint64_t count)
{
    if (offp && !uptr_ok_w(offp, sizeof(*offp)))
        return -(int64_t) EFAULT;
    vfs_file_t* inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG)
        return -(int64_t) EINVAL;
    vfs_node_t* src = inf->node;
    uint64_t off = offp ? *offp : inf->pos;
    if (off >= src->size)
        return 0;
    uint64_t avail = src->size - off;
    if (count > avail)
        count = avail;
    int64_t w = fd_write_kbuf(outfd, src->data + off, count);
    if (w > 0) {
        if (offp)
            *offp = off + (uint64_t) w;
        else
            inf->pos = off + (uint64_t) w;
    }
    return w;
}

int64_t sys_preadv(int fd, const struct iovec* iov, int n, uint64_t off)
{
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r = fd_pread(fd, (void*) iov[i].iov_base, iov[i].iov_len, off + (uint64_t) total);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t) r < iov[i].iov_len)
            break;
    }
    return total;
}

int64_t sys_pwritev(int fd, const struct iovec* iov, int n, uint64_t off)
{
    int64_t total = 0;
    for (int i = 0; i < n; i++) {
        int64_t r =
            fd_pwrite(fd, (const void*) iov[i].iov_base, iov[i].iov_len, off + (uint64_t) total);
        if (r < 0)
            return total ? total : r;
        total += r;
    }
    return total;
}

static uint32_t g_memfd_seq;

int64_t sys_memfd_create(const char* name, uint32_t flags)
{
    (void) name;
    (void) flags;
    char path[32];
    snprintf(path, sizeof(path), "/tmp/.mfd%u", ++g_memfd_seq);
    return fd_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
}

int64_t sys_copy_file_range(int infd, uint64_t* off_in, int outfd, uint64_t* off_out, uint64_t len,
                            uint32_t flags)
{
    (void) flags;
    if (off_in && !uptr_ok_w(off_in, sizeof(*off_in)))
        return -(int64_t) EFAULT;
    if (off_out && !uptr_ok_w(off_out, sizeof(*off_out)))
        return -(int64_t) EFAULT;
    vfs_file_t* inf = fd_get_file(infd);
    if (!inf || !inf->node || inf->node->type != VFS_TYPE_REG)
        return -(int64_t) EINVAL;
    vfs_node_t* src = inf->node;
    uint64_t rin = off_in ? *off_in : inf->pos;
    if (rin >= src->size)
        return 0;
    uint64_t avail = src->size - rin;
    if (len > avail)
        len = avail;
    uint64_t rout = off_out ? *off_out : (fd_get_file(outfd) ? fd_get_file(outfd)->pos : 0);
    int64_t w = fd_pwrite(outfd, src->data + rin, len, rout);
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

int64_t sys_statx(int dirfd, const char* path, int flags, uint32_t mask, struct statx* sx)
{
    (void) mask;
    if (!sx)
        return -(int64_t) EFAULT;
    if (!uptr_ok_w(sx, sizeof(*sx)))
        return -(int64_t) EFAULT;
    vfs_node_t* n = NULL;
    if (!path || path[0] == '\0') {
        n = fd_get_node(dirfd);
    } else {
        char abs[512];
        at_resolve(dirfd, path, abs, sizeof(abs));
        n = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(abs) : vfs_lookup(abs);
    }
    if (!n)
        return -(int64_t) ENOENT;
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
    return 0;
}
