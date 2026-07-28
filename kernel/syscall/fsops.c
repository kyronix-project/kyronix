#include "fsops.h"

#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "internal.h"
#include "lib/string.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "version.h"

static spinlock_t hostname_lock;
static char system_hostname[65] = "kx";

uint64_t system_hostname_copy(char *out, uint64_t capacity) {
    if (!out || capacity == 0) return 0;
    spin_lock(&hostname_lock);
    uint64_t length = strlen(system_hostname);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, system_hostname, length);
    out[length] = '\0';
    spin_unlock(&hostname_lock);
    return length;
}

int64_t sys_uname(struct utsname *buf) {
    if (!buf) return -(int64_t) EFAULT;
    if (!uptr_ok_w(buf, sizeof(*buf))) return -(int64_t) EFAULT;
    memset(buf, 0, sizeof(*buf));
    memcpy(buf->sysname, "k9", 3);
    system_hostname_copy(buf->nodename, sizeof(buf->nodename));
    memcpy(buf->release, KERNEL_VERSION, sizeof(KERNEL_VERSION));
    memcpy(buf->version, "#1 SMP", 7);
    memcpy(buf->machine, "x86_64", 7);
    return 0;
}

int64_t sys_sethostname(const char *name, uint64_t length) {
    if (length == 0 || length > sizeof(system_hostname) - 1)
        return -(int64_t) EINVAL;
    if (!name || !uptr_ok(name, length)) return -(int64_t) EFAULT;

    spin_lock(&hostname_lock);
    memcpy(system_hostname, name, length);
    system_hostname[length] = '\0';
    spin_unlock(&hostname_lock);
    return 0;
}

int64_t sys_getcwd(char *buf, uint64_t size) {
    if (!buf || !size) return -(int64_t) EINVAL;
    if (!uptr_ok_w(buf, size)) return -(int64_t) EFAULT;
    proc_t *p = cur();
    char tmp[512];
    strncpy(tmp, p ? p->cwd : g_cwd, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    jail_strip_root(tmp, sizeof(tmp)); /* report jail-relative cwd to the process */
    size_t len = strlen(tmp) + 1;
    if (len > size) return -(int64_t) EINVAL;
    memcpy(buf, tmp, len);
    return (int64_t) (uintptr_t) buf;
}

int64_t sys_chdir(const char *path) {
    if (!path) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    const char *rp = abs;
    vfs_node_t *n = vfs_lookup(rp);
    if (!n) return -(int64_t) ENOENT;
    if (n->type != VFS_TYPE_DIR) {
        vfs_node_unref_internal(n);
        return -(int64_t) ENOTDIR;
    }
    proc_t *p = cur();
    if (p) strncpy(p->cwd, rp, sizeof(p->cwd) - 1);
    vfs_node_unref_internal(n);
    return 0;
}

int64_t sys_mkdir(const char *path, uint32_t mode) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_mkdir(abs, mode);
}

int64_t sys_rmdir(const char *path) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_rmdir(abs);
}

int64_t sys_unlink(const char *path) {
    if (!path) return -(int64_t) EINVAL;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_unlink(abs);
}

int64_t sys_rename(const char *oldpath, const char *newpath) {
    if (!oldpath || !newpath) return -(int64_t) EINVAL;
    char abs_old[512], abs_new[512];
    if (!path_abs(abs_old, oldpath) || !path_abs(abs_new, newpath)) return -(int64_t) EFAULT;
    return (int64_t) vfs_rename(abs_old, abs_new);
}

int64_t sys_umask(uint64_t mask) {
    proc_t *p = cur();
    if (!p) return 0022;
    uint32_t old = p->umask;
    p->umask = (uint32_t) mask & 0777U;
    return (int64_t) old;
}

int64_t sys_ftruncate(int fd, uint64_t len) {
    vfs_file_t *file = fd_get_file(fd);
    if (!file) return -(int64_t) EBADF;
    if ((file->flags & O_ACCMODE) == O_RDONLY) return -(int64_t) EBADF;
    vfs_node_t *n = file->node;
    if (!n) return -(int64_t) EBADF;
    if (n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (len < n->size) n->size = len;
    return 0;
}

int64_t sys_truncate(const char *path, uint64_t len) {
    if (!path) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, path)) return -(int64_t) EFAULT;
    return (int64_t) vfs_truncate(abs, len);
}

int64_t sys_symlink(const char *target, const char *linkpath) {
    if (!target || !linkpath) return -(int64_t) EFAULT;
    char abs[512], ktarget[512];
    if (!vfs_copy_user_path(target, ktarget)) return -(int64_t) EFAULT;
    if (!path_abs(abs, linkpath)) return -(int64_t) EFAULT;
    vfs_node_t *n = vfs_create_symlink(abs, ktarget);
    return n ? 0 : -(int64_t) EEXIST;
}

int64_t sys_statfs(const char *path, void *buf) {
    (void) path;
    if (buf) {
        if (!uptr_ok_w(buf, 120)) return -(int64_t) EFAULT;
        memset(buf, 0, 120);
        uint64_t *w = (uint64_t *) buf; /* Linux struct statfs */
        w[0] = 0x858458f6;              /* f_type = RAMFS_MAGIC */
        w[1] = 4096;                    /* f_bsize  */
        w[2] = 65536;                   /* f_blocks */
        w[3] = 65536;                   /* f_bfree  */
        w[4] = 65536;                   /* f_bavail */
        w[5] = 4096;                    /* f_files  */
        w[6] = 4096;                    /* f_ffree  */
        w[8] = 255;                     /* f_namelen */
        w[9] = 4096;                    /* f_frsize  */
    }
    return 0;
}

int64_t sys_access(const char *p, int m) {
    if (!p) return -(int64_t) EFAULT;
    char abs[512];
    if (!path_abs(abs, p)) return -(int64_t) EFAULT;
    return (int64_t) vfs_access(abs, m);
}

int64_t sys_fchdir(int fd) {
    vfs_node_t *n = fd_get_node(fd);
    if (!n) return -(int64_t) EBADF;
    if (n->type != VFS_TYPE_DIR) return -(int64_t) ENOTDIR;
    proc_t *p = cur();
    if (!p) return 0;
    char buf[512];
    if (vfs_node_abspath(n, buf, sizeof(buf))) strncpy(p->cwd, buf, sizeof(p->cwd) - 1);
    return 0;
}

int64_t sys_link(const char *old, const char *lnew) {
    if (!old || !lnew) return -(int64_t) EFAULT;
    char abs_old[512], abs_new[512];
    if (!path_abs(abs_old, old) || !path_abs(abs_new, lnew)) return -(int64_t) EFAULT;
    return (int64_t) vfs_link(abs_old, abs_new);
}
