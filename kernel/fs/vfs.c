#include "vfs.h"
#include "arch/x86_64/cpu.h"
#include "devfs.h"
#include "drivers/tty.h"
#include "eventfd.h"
#include "inet_socket.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/jail.h"
#include "proc/proc.h"
#include "procfs.h"
#include "security/anti_toctou.h"
#include "security/phantom.h"
#include "syscall/syscall.h"
#include "unix_socket.h"

extern volatile uint64_t g_ticks;
static vfs_node_t *g_root = NULL;
static uint32_t g_next_ino = 1;

char g_cwd[512] = "/";

static vfs_file_t *g_default_fds[VFS_FD_MAX];

static inline vfs_file_t **vfs_cur_fds(void) { return g_cur_fds ? g_cur_fds : g_default_fds; }

#define FS_MAX 8
static struct filesystem *g_filesystems[FS_MAX];
static int g_filesystem_cnt;

#define VFS_FILE_MAGIC 0x4b59464d41474943ULL

#define EACCES 13
#define EFAULT 14
#define EEXIST 17
#define ENOENT 2
#define EBADF 9
#define ENOMEM 12
#define ENOTDIR 20
#define EISDIR 21
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define EFBIG 27
#define ENOSPC 28
#define ESPIPE 29
#define ENOTEMPTY 39
#define ENAMETOOLONG 36
#define EAGAIN 11
#define EPERM 1
#define ECONNREFUSED 111
#define ENOTCONN 107
#define EISCONN 106
#define EADDRINUSE 98

static vfs_node_t *node_alloc(const char *name, uint8_t type, uint32_t mode) {
    vfs_node_t *n = (vfs_node_t *) kcalloc(1, sizeof(vfs_node_t));
    if (!n) return NULL;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = type;
    n->mode = mode;
    uint64_t _nf = irq_save();
    n->ino = g_next_ino++;
    irq_restore(_nf);
    if (g_current_proc) {
        n->uid = g_current_proc->fsuid;
        n->gid = g_current_proc->fsgid;
    }
    return n;
}

static void dir_insert_nolock(vfs_node_t *dir, vfs_node_t *child) {
    child->parent = dir;
    child->next = dir->children;
    dir->children = child;
}

static void dir_insert(vfs_node_t *dir, vfs_node_t *child) {
    uint64_t f = irq_save();
    dir_insert_nolock(dir, child);
    irq_restore(f);
}

vfs_node_t *vfs_node_alloc_internal(const char *name, uint8_t type, uint32_t mode) {
    return node_alloc(name, type, mode);
}

void vfs_dir_insert_internal(vfs_node_t *dir, vfs_node_t *child) { dir_insert(dir, child); }

static vfs_node_t *dir_find(vfs_node_t *dir, const char *name) {
    for (vfs_node_t *c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

vfs_node_t *vfs_dir_find_internal(vfs_node_t *dir, const char *name) { return dir_find(dir, name); }

static vfs_node_t *lookup_ref(vfs_node_t *n) {
    if (!n) return NULL;
    uint64_t f = irq_save();
    if (n->deleted)
        n = NULL;
    else
        n->refcnt++;
    irq_restore(f);
    return n;
}

static void dir_remove(vfs_node_t *parent, vfs_node_t *child) {
    uint64_t f = irq_save();
    if (parent->children == child) {
        parent->children = child->next;
    } else {
        for (vfs_node_t *c = parent->children; c; c = c->next)
            if (c->next == child) {
                c->next = child->next;
                break;
            }
    }
    child->next = NULL;
    child->parent = NULL;
    irq_restore(f);
}

static void node_destroy(vfs_node_t *n) {
    if (!n) return;
    if (n->fs_ops && n->fs_ops->close) n->fs_ops->close(n);
    if (n->type == VFS_TYPE_REG && n->data) kfree(n->data);
    if (n->type == VFS_TYPE_SYM && n->symlink) kfree(n->symlink);
    kfree(n);
}

static void node_ref(vfs_node_t *n) {
    if (!n) return;
    uint64_t f = irq_save();
    n->refcnt++;
    irq_restore(f);
}

static void node_unref(vfs_node_t *n) {
    if (!n) return;
    bool destroy = false;
    uint64_t f = irq_save();
    if (n->refcnt && --n->refcnt == 0 && n->deleted) destroy = true;
    irq_restore(f);
    if (destroy) node_destroy(n);
}

static void node_unlink_or_destroy(vfs_node_t *n) {
    if (!n) return;
    n->deleted = 1;
    n->next = NULL;
    n->parent = NULL;
    if (n->refcnt == 0) node_destroy(n);
}

void vfs_node_ref_internal(vfs_node_t *n) { node_ref(n); }

void vfs_node_unref_internal(vfs_node_t *n) { node_unref(n); }

void vfs_node_mark_deleted_internal(vfs_node_t *n) {
    if (n) n->deleted = 1;
}

static uint32_t cred_fsuid(void) { return g_current_proc ? g_current_proc->fsuid : 0; }

static uint32_t cred_fsgid(void) { return g_current_proc ? g_current_proc->fsgid : 0; }

static bool cred_is_root(void) { return !g_current_proc || g_current_proc->euid == 0; }

static bool cred_fsroot(void) { return !g_current_proc || g_current_proc->fsuid == 0; }

static bool may_access(vfs_node_t *n, uint32_t need) {
    if (!n) return false;
    if (cred_fsroot()) return true;
    uint32_t bits;
    if (n->uid == cred_fsuid())
        bits = (n->mode >> 6) & 7u;
    else if (n->gid == cred_fsgid())
        bits = (n->mode >> 3) & 7u;
    else
        bits = n->mode & 7u;
    return (bits & need) == need;
}

static bool may_create_in(vfs_node_t *dir) {
    return dir && dir->type == VFS_TYPE_DIR && may_access(dir, 3u);
}

bool vfs_may_create_in_internal(vfs_node_t *dir) { return may_create_in(dir); }

/* stickybit deletion check: in a sticky dir , a non-root caller may
   only remove/rename an entry it owns, or whose containing dir it owns. */
static bool may_delete_in(vfs_node_t *dir, vfs_node_t *victim) {
    if (!may_create_in(dir)) return false;
    if (!(dir->mode & S_ISVTX)) return true;
    if (cred_fsroot()) return true;
    uint32_t u = cred_fsuid();
    return (victim && victim->uid == u) || dir->uid == u;
}

static uint32_t mode_without_priv_bits(uint32_t mode) {
    return cred_is_root() ? (mode & 07777U) : (mode & 01777U);
}

static uint32_t apply_umask(uint32_t mode) {
    return g_current_proc ? (mode & ~g_current_proc->umask) : mode;
}

static bool may_change_owner(vfs_node_t *n) {
    (void) n;
    return cred_is_root();
}

static bool may_change_mode(vfs_node_t *n) {
    return n && (cred_is_root() || n->uid == cred_fsuid());
}

static void vfs_abs_path(char *out, size_t sz, const char *in) {
    const char *root = jail_root_current();
    if (!in || in[0] == '/') {
        if (root[0])
            snprintf(out, sz, "%s%s", root, in ? in : "");
        else {
            strncpy(out, in ? in : "", sz - 1);
            out[sz - 1] = '\0';
        }
    } else {
        const char *cwd = (g_current_proc && g_current_proc->cwd[0]) ? g_current_proc->cwd : g_cwd;
        size_t cl = strlen(cwd);
        if (cl >= sz) {
            out[0] = '\0';
            return;
        }
        memcpy(out, cwd, cl);
        if (out[cl - 1] != '/') out[cl++] = '/';
        strncpy(out + cl, in, sz - cl - 1);
        out[sz - 1] = '\0';
    }
    if (root[0]) jail_canon_clamp(out, sz, root);
}

static int vfs_node_path(vfs_node_t *n, char *buf, size_t sz) {
    if (!n || !buf || sz == 0) return -1;
    if (n->parent == n) {
        if (sz < 2) return -1;
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    vfs_node_t *stack[128];
    int depth = 0;
    for (vfs_node_t *cur = n; cur && cur->parent != cur && depth < 128; cur = cur->parent)
        stack[depth++] = cur;
    if (depth >= 128) return -1;

    size_t pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--) {
        if (pos > 1) buf[pos++] = '/';
        size_t len = strlen(stack[i]->name);
        if (pos + len >= sz) return -1;
        memcpy(buf + pos, stack[i]->name, len);
        pos += len;
    }
    buf[pos] = '\0';
    return 0;
}

static vfs_node_t *lookup_internal(const char *path, bool follow_last, int depth) {
    if (!path || path[0] == '\0') return NULL;
    if (depth > 32) return NULL;

    vfs_node_t *cur;
    if (path[0] == '/') {
        cur = g_root;
    } else {
        const char *cwd = (g_current_proc && g_current_proc->cwd[0]) ? g_current_proc->cwd : g_cwd;
        size_t cwd_len = strlen(cwd);
        char full[512];
        if (cwd_len + 1 + strlen(path) >= sizeof(full)) return NULL;
        memcpy(full, cwd, cwd_len);
        if (full[cwd_len - 1] != '/') full[cwd_len++] = '/';
        strcpy(full + cwd_len, path);
        return lookup_internal(full, follow_last, depth + 1);
    }
    if (!cur) return NULL;

    const char *p = path;
    while (*p == '/') p++;
    if (*p == '\0') return lookup_ref(cur);

    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - start);
        while (*p == '/') p++;

        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (cur->parent) cur = cur->parent;
            continue;
        }

        char comp[256];
        if (len >= sizeof(comp)) return NULL;
        memcpy(comp, start, len);
        comp[len] = '\0';

        bool last = (*p == '\0');
        if (cur->type != VFS_TYPE_DIR) return NULL;
        if (!may_access(cur, 1u)) return NULL;
        vfs_node_t *child = dir_find(cur, comp);
        if (!child) {
            if (cur->parent == g_root && strcmp(cur->name, "proc") == 0)
                procfs_try_pid_dir(cur, comp);
            child = dir_find(cur, comp);
            if (!child) return NULL;
        }

        if (child->type == VFS_TYPE_SYM && (follow_last || !last)) {
            if (!child->symlink) return NULL;

            char resolved[512];
            if (child->symlink[0] == '/') {
                if (*p)
                    snprintf(resolved, sizeof(resolved), "%s/%s", child->symlink, p);
                else
                    snprintf(resolved, sizeof(resolved), "%s", child->symlink);
            } else {
                char base[512];
                if (vfs_node_path(child->parent, base, sizeof(base)) < 0) return NULL;
                if (strcmp(base, "/") == 0) {
                    if (*p)
                        snprintf(resolved, sizeof(resolved), "/%s/%s", child->symlink, p);
                    else
                        snprintf(resolved, sizeof(resolved), "/%s", child->symlink);
                } else {
                    if (*p)
                        snprintf(resolved, sizeof(resolved), "%s/%s/%s", base, child->symlink, p);
                    else
                        snprintf(resolved, sizeof(resolved), "%s/%s", base, child->symlink);
                }
            }
            return lookup_internal(resolved, true, depth + 1);
        }
        cur = child;
    }
    return lookup_ref(cur);
}

vfs_node_t *vfs_lookup(const char *path) { return lookup_internal(path, true, 0); }
vfs_node_t *vfs_lookup_nofollow(const char *path) { return lookup_internal(path, false, 0); }

vfs_node_t *vfs_mkdir_p(const char *path, uint32_t mode) {
    if (!path || path[0] != '/') return NULL;
    vfs_node_t *cur = g_root;
    const char *p = path + 1;
    while (*p) {
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - start);
        while (*p == '/') p++;
        if (len == 0) continue;
        char comp[256];
        if (len >= sizeof(comp)) return NULL;
        memcpy(comp, start, len);
        comp[len] = '\0';
        vfs_node_t *newchild = node_alloc(comp, VFS_TYPE_DIR, mode | S_IFDIR);
        if (!newchild) return NULL;
        uint64_t _f = irq_save();
        vfs_node_t *child = dir_find(cur, comp);
        if (!child) {
            dir_insert_nolock(cur, newchild);
            irq_restore(_f);
            child = newchild;
        } else {
            irq_restore(_f);
            kfree(newchild);
        }
        if (child->type != VFS_TYPE_DIR) return NULL;
        cur = child;
    }
    return cur;
}

static vfs_node_t *parent_of(const char *path, const char **leaf) {
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/') len--;
    const char *slash = NULL;
    for (size_t i = 0; i < len; i++)
        if (path[i] == '/') slash = path + i;
    if (!slash || slash == path) {
        *leaf = path + (path[0] == '/' ? 1 : 0);
        return lookup_ref(g_root);
    }
    size_t plen = (size_t) (slash - path);
    char parent_path[512];
    if (plen >= sizeof(parent_path)) return NULL;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    *leaf = slash + 1;
    return vfs_lookup(plen ? parent_path : "/");
}

vfs_node_t *vfs_create_file(const char *path, uint32_t mode, const void *data, uint64_t size) {
    const char *leaf;
    vfs_node_t *parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR) {
        node_unref(parent);
        return NULL;
    }

    vfs_node_t *n = node_alloc(leaf, VFS_TYPE_REG, apply_umask(mode & 07777) | S_IFREG);
    if (!n) {
        node_unref(parent);
        return NULL;
    }
    if (size > 0) {
        n->data = (uint8_t *) kmalloc(size);
        if (!n->data) {
            kfree(n);
            node_unref(parent);
            return NULL;
        }
        memcpy(n->data, data, size);
        n->size = n->capacity = size;
    }

    uint64_t _f = irq_save();
    vfs_node_t *existing = dir_find(parent, leaf);
    if (existing) {
        irq_restore(_f);
        if (n->data) kfree(n->data);
        kfree(n);
        if (existing->type != VFS_TYPE_REG) {
            node_unref(parent);
            return NULL;
        }
        kfree(existing->data);
        existing->data = NULL;
        existing->size = existing->capacity = 0;
        if (size > 0) {
            existing->data = (uint8_t *) kmalloc(size);
            if (!existing->data) {
                node_unref(parent);
                return NULL;
            }
            memcpy(existing->data, data, size);
            existing->size = existing->capacity = size;
        }
        node_unref(parent);
        return existing;
    }
    dir_insert_nolock(parent, n);
    irq_restore(_f);
    node_unref(parent);
    return n;
}

vfs_node_t *vfs_create_symlink(const char *path, const char *target) {
    const char *leaf;
    vfs_node_t *parent = parent_of(path, &leaf);
    if (!parent) return NULL;
    if (!may_create_in(parent)) {
        node_unref(parent);
        return NULL;
    }
    vfs_node_t *n = node_alloc(leaf, VFS_TYPE_SYM, 0777 | S_IFLNK);
    if (!n) {
        node_unref(parent);
        return NULL;
    }
    size_t tlen = strnlen(target, 511);
    n->symlink = (char *) kmalloc(tlen + 1);
    if (!n->symlink) {
        kfree(n);
        node_unref(parent);
        return NULL;
    }
    memcpy(n->symlink, target, tlen);
    n->symlink[tlen] = '\0';
    n->size = tlen;
    uint64_t _f = irq_save();
    if (dir_find(parent, leaf)) {
        irq_restore(_f);
        kfree(n->symlink);
        kfree(n);
        node_unref(parent);
        return NULL;
    }
    dir_insert_nolock(parent, n);
    irq_restore(_f);
    node_unref(parent);
    return n;
}

vfs_node_t *vfs_create_chr(const char *path,
                           int64_t (*rfn)(vfs_node_t *, char *, uint64_t, uint64_t),
                           int64_t (*wfn)(vfs_node_t *, const char *, uint64_t, uint64_t)) {
    const char *leaf;
    vfs_node_t *parent = parent_of(path, &leaf);
    if (!parent) return NULL;
    vfs_node_t *n = node_alloc(leaf, VFS_TYPE_CHR, 0666 | S_IFCHR);
    if (!n) {
        node_unref(parent);
        return NULL;
    }
    n->chr_read = rfn;
    n->chr_write = wfn;
    uint64_t _f = irq_save();
    if (dir_find(parent, leaf)) {
        irq_restore(_f);
        kfree(n);
        node_unref(parent);
        return NULL;
    }
    dir_insert_nolock(parent, n);
    irq_restore(_f);
    node_unref(parent);
    return n;
}

int vfs_mkdir(const char *path, uint32_t mode) {
    const char *leaf;
    vfs_node_t *parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR || !leaf || !*leaf) {
        node_unref(parent);
        return -(int) ENOENT;
    }
    if (!may_create_in(parent)) {
        node_unref(parent);
        return -(int) EACCES;
    }
    vfs_node_t *n = node_alloc(leaf, VFS_TYPE_DIR, apply_umask(mode & 07777) | S_IFDIR);
    if (!n) {
        node_unref(parent);
        return -(int) ENOMEM;
    }
    uint64_t _f = irq_save();
    if (dir_find(parent, leaf)) {
        irq_restore(_f);
        kfree(n);
        node_unref(parent);
        return -(int) EEXIST;
    }
    dir_insert_nolock(parent, n);
    irq_restore(_f);
    node_unref(parent);
    return 0;
}

int vfs_unlink(const char *path) {
    vfs_node_t *n = vfs_lookup_nofollow(path);
    if (!n) return -(int) ENOENT;
    if (n->type == VFS_TYPE_DIR) {
        node_unref(n);
        return -(int) EISDIR;
    }
    if (!n->parent) {
        node_unref(n);
        return -(int) EINVAL;
    }
    if (!may_delete_in(n->parent, n)) {
        node_unref(n);
        return -(int) EACCES;
    }
    dir_remove(n->parent, n);
    node_unlink_or_destroy(n);
    node_unref(n);
    return 0;
}

int vfs_rmdir(const char *path) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -(int) ENOENT;
    if (n->type != VFS_TYPE_DIR) {
        node_unref(n);
        return -(int) ENOTDIR;
    }
    if (n->children) {
        node_unref(n);
        return -(int) ENOTEMPTY;
    }
    if (!n->parent) {
        node_unref(n);
        return -(int) EINVAL;
    }
    if (!may_delete_in(n->parent, n)) {
        node_unref(n);
        return -(int) EACCES;
    }
    dir_remove(n->parent, n);
    node_unlink_or_destroy(n);
    node_unref(n);
    return 0;
}

int vfs_rename(const char *oldpath, const char *newpath) {
    vfs_node_t *n = vfs_lookup_nofollow(oldpath);
    if (!n || !n->parent) {
        node_unref(n);
        return -(int) ENOENT;
    }
    const char *new_leaf;
    vfs_node_t *new_parent = parent_of(newpath, &new_leaf);
    if (!new_parent || new_parent->type != VFS_TYPE_DIR) {
        node_unref(n);
        node_unref(new_parent);
        return -(int) ENOENT;
    }
    if (!new_leaf || !*new_leaf) {
        node_unref(n);
        node_unref(new_parent);
        return -(int) EINVAL;
    }
    if (!may_delete_in(n->parent, n) || !may_create_in(new_parent)) {
        node_unref(n);
        node_unref(new_parent);
        return -(int) EACCES;
    }

    uint64_t _f = irq_save();
    vfs_node_t *existing = dir_find(new_parent, new_leaf);
    if (existing) existing->refcnt++;
    irq_restore(_f);

    if (existing && !may_delete_in(new_parent, existing)) {
        node_unref(existing);
        node_unref(n);
        node_unref(new_parent);
        return -(int) EACCES;
    }

    if (existing) {
        if (existing->type == VFS_TYPE_DIR) {
            if (n->type != VFS_TYPE_DIR) {
                node_unref(existing);
                node_unref(n);
                node_unref(new_parent);
                return -(int) EISDIR;
            }
            if (existing->children) {
                node_unref(existing);
                node_unref(n);
                node_unref(new_parent);
                return -(int) ENOTEMPTY;
            }
        } else if (n->type == VFS_TYPE_DIR) {
            node_unref(existing);
            node_unref(n);
            node_unref(new_parent);
            return -(int) ENOTDIR;
        }
        dir_remove(new_parent, existing);
        node_unlink_or_destroy(existing);
        node_unref(existing);
    }
    dir_remove(n->parent, n);
    strncpy(n->name, new_leaf, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    dir_insert(new_parent, n);
    node_unref(n);
    node_unref(new_parent);
    return 0;
}

static int fd_alloc_from(int start) {
    vfs_file_t **fds = vfs_cur_fds();
    for (int i = start; i < VFS_FD_MAX; i++)
        if (!fds[i]) return i;
    return -1;
}

static vfs_file_t *file_alloc(void) {
    vfs_file_t *f = (vfs_file_t *) kcalloc(1, sizeof(vfs_file_t));
    if (f) f->magic = VFS_FILE_MAGIC;
    return f;
}

static bool file_valid(vfs_file_t *f) {
    uintptr_t addr = (uintptr_t) f;
    if (!f || addr < HEAP_START || addr >= HEAP_MAX || (addr & 7)) return false;
    return f->magic == VFS_FILE_MAGIC;
}

static vfs_file_t *fd_get(int fd) {
    if (fd < 0 || fd >= VFS_FD_MAX) return NULL;
    vfs_file_t *f = vfs_cur_fds()[fd];
    if (!file_valid(f)) return NULL;
    return f;
}

int vfs_fd_alloc_from(int start) { return fd_alloc_from(start); }

vfs_file_t *vfs_file_alloc(void) { return file_alloc(); }

vfs_file_t *vfs_fd_get(int fd) { return fd_get(fd); }

void vfs_fd_install(int fd, vfs_file_t *f) {
    if (fd >= 0 && fd < VFS_FD_MAX) vfs_cur_fds()[fd] = f;
}

void vfs_fd_clear(int fd) {
    if (fd >= 0 && fd < VFS_FD_MAX) vfs_cur_fds()[fd] = NULL;
}

bool fd_valid(int fd) { return fd_get(fd) != NULL; }

vfs_node_t *fd_get_node(int fd) {
    vfs_file_t *f = fd_get(fd);
    return f ? f->node : NULL;
}

vfs_file_t *fd_get_file(int fd) { return fd_get(fd); }

int fd_open_node(vfs_node_t *n, int flags) {
    if (!n) return -(int) ENOENT;
    int fd = fd_alloc_from(0);
    if (fd < 0) return -(int) EMFILE;
    vfs_file_t *f = file_alloc();
    if (!f) return -(int) ENOMEM;
    f->node = n;
    f->flags = flags;
    node_ref(n);
    vfs_cur_fds()[fd] = f;
    return fd;
}

int64_t fd_pread(int fd, void *buf, uint64_t len, uint64_t off) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->pipe) return -(int64_t) ESPIPE;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -(int64_t) EBADF;
    if (len == 0) return 0;
    if (!uptr_ok_w(buf, len)) return -(int64_t) EFAULT; /* kernel writes into buf */
    return fd_pread_kbuf(fd, buf, len, off);
}

int64_t fd_pread_kbuf(int fd, void *buf, uint64_t len, uint64_t off) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->pipe) return -(int64_t) ESPIPE;
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -(int64_t) EBADF;
    if (len == 0) return 0;
    vfs_node_t *n = f->node;
    if (!n || n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (n->fs_ops && n->fs_ops->read) return n->fs_ops->read(n, (char *) buf, off, len);
    if (off >= n->size) return 0;
    uint64_t avail = n->size - off;
    uint64_t r = (len < avail) ? len : avail;
    memcpy(buf, n->data + off, r);
    return (int64_t) r;
}

int64_t fd_pwrite(int fd, const void *buf, uint64_t len, uint64_t off) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->pipe) return -(int64_t) ESPIPE;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -(int64_t) EBADF;
    if (len == 0) return 0;
    if (!uptr_ok(buf, len)) return -(int64_t) EFAULT;
    vfs_node_t *n = f->node;
    if (!n || n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (off > INT64_MAX) return -(int64_t) EINVAL;
    if (len > (uint64_t) INT64_MAX - off) return -(int64_t) EFBIG;
    if (n->fs_ops && n->fs_ops->write) return n->fs_ops->write(n, (const char *) buf, off, len);
    uint64_t end = off + len;
    if (end > n->capacity) {
        if (end > UINT64_MAX - 4095) return -(int64_t) EFBIG;
        uint64_t newcap = (end + 4095) & ~4095ULL;
        uint8_t *newdata = (uint8_t *) kmalloc(newcap);
        if (!newdata) return -(int64_t) ENOSPC;
        if (n->data) {
            memcpy(newdata, n->data, n->size);
            kfree(n->data);
        }
        n->data = newdata;
        n->capacity = newcap;
    }
    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    n->dirty = 1;
    return (int64_t) len;
}

int64_t fd_pwrite_kbuf(int fd, const void *buf, uint64_t len, uint64_t off) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->pipe) return -(int64_t) ESPIPE;
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -(int64_t) EBADF;
    if (len == 0) return 0;
    vfs_node_t *n = f->node;
    if (!n || n->type != VFS_TYPE_REG) return -(int64_t) EINVAL;
    if (off > INT64_MAX) return -(int64_t) EINVAL;
    if (len > (uint64_t) INT64_MAX - off) return -(int64_t) EFBIG;
    if (n->fs_ops && n->fs_ops->write) return n->fs_ops->write(n, (const char *) buf, off, len);
    uint64_t end = off + len;
    if (end > n->capacity) {
        if (end > UINT64_MAX - 4095) return -(int64_t) EFBIG;
        uint64_t newcap = (end + 4095) & ~4095ULL;
        uint8_t *newdata = (uint8_t *) kmalloc(newcap);
        if (!newdata) return -(int64_t) ENOSPC;
        if (n->data) {
            memcpy(newdata, n->data, n->size);
            kfree(n->data);
        }
        n->data = newdata;
        n->capacity = newcap;
    }
    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    n->dirty = 1;
    return (int64_t) len;
}

bool fd_pollin(int fd) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return false;
    if (f->efd) return f->efd->counter > 0;
    if (f->tfd) return f->tfd->next_tick && g_ticks >= f->tfd->next_tick;
    if (f->inet) return inet_poll_in(f->inet);
    if (f->wpipe) /* socket - readable when read-pipe has data */
        return f->pipe->count > 0 || f->pipe->write_refs == 0;
    if (f->pipe)
        return f->pipe_end == PIPE_END_READ && (f->pipe->count > 0 || f->pipe->write_refs == 0);
    if (!f->node) return false;
    if (f->node->type == VFS_TYPE_SOCK) return f->node->sock_backlog > 0;
    if (f->node->type == VFS_TYPE_CHR) {
        if (f->node->chr_pollin) return f->node->chr_pollin(f->node);
        return tty_data_ready();
    }
    return true;
}

bool fd_pollout(int fd) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return false;
    if (f->efd) return f->efd->counter < UINT64_MAX - 1u;
    if (f->tfd) return false;
    if (f->inet) return inet_poll_out(f->inet);
    if (f->wpipe) /* socket - writable when write-pipe has space */
        return f->wpipe->count < PIPE_BUFSZ && f->wpipe->read_refs > 0;
    if (f->node && f->node->type == VFS_TYPE_SOCK) return false;
    if (f->pipe)
        return f->pipe_end == PIPE_END_WRITE && f->pipe->count < PIPE_BUFSZ &&
               f->pipe->read_refs > 0;
    return f->node != NULL;
}

/* POLLHUP/EPOLLHUP: a pipe/socket read end whose writers have all gone away */
bool fd_pollhup(int fd) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return false;
    if (f->wpipe) return f->pipe->write_refs == 0;
    if (f->pipe) return f->pipe_end == PIPE_END_READ && f->pipe->write_refs == 0;
    return false;
}

uint64_t fd_poll_deadline(int fd) {
    vfs_file_t *f = fd_get(fd);
    return (f && f->tfd && f->tfd->next_tick) ? f->tfd->next_tick : UINT64_MAX;
}

static void pipe_drop_write(pipe_t *p) {
    if (!p) return;
    if (p->write_refs) p->write_refs--;
    if (p->write_refs == 0) pipe_wake(p, 1); /* EOF: wake all blocked readers */
}

void vfs_pipe_drop_write(pipe_t *p) { pipe_drop_write(p); }

static void pipe_maybe_free(pipe_t *p) {
    if (p && p->read_refs == 0 && p->write_refs == 0) pipe_free(p);
}

void vfs_pipe_maybe_free(pipe_t *p) { pipe_maybe_free(p); }

static void file_close(vfs_file_t *f) {
    if (!file_valid(f)) return;
    if (f->efd) {
        if (__sync_sub_and_fetch(&f->efd->refcnt, 1) == 0) kfree(f->efd);
        f->magic = 0;
        kfree(f);
        return;
    }
    if (f->tfd) {
        if (__sync_sub_and_fetch(&f->tfd->refcnt, 1) == 0) kfree(f->tfd);
        f->magic = 0;
        kfree(f);
        return;
    }
    if (f->inet) {
        inet_conn_close(f->inet);
        f->magic = 0;
        kfree(f);
        return;
    }
    if (!f->pipe && !f->wpipe && f->node && f->node->type == VFS_TYPE_SOCK) {
        unix_socket_close(f);
        node_unref(f->node);
        f->magic = 0;
        kfree(f);
        return;
    }
    if (f->wpipe) {
        if (f->pipe->read_refs) f->pipe->read_refs--;
        pipe_maybe_free(f->pipe);
        pipe_drop_write(f->wpipe);
        pipe_maybe_free(f->wpipe);
    } else if (f->pipe) {
        if (f->pipe_end == PIPE_END_READ) {
            if (f->pipe->read_refs) f->pipe->read_refs--;
        } else {
            pipe_drop_write(f->pipe);
        }
        pipe_maybe_free(f->pipe);
    }
    if (f->node) {
        vfs_node_t *n = f->node;
        void (*cc)(vfs_node_t *) = (n->type == VFS_TYPE_CHR) ? n->chr_close : NULL;
        if (cc && n->refcnt <= 1) cc(n);
        node_unref(n);
    }
    f->magic = 0;
    kfree(f);
}

void vfs_file_close(vfs_file_t *f) { file_close(f); }

static void file_addref(vfs_file_t *f) {
    if (!f) return;
    if (f->efd) __sync_add_and_fetch(&f->efd->refcnt, 1);
    if (f->tfd) __sync_add_and_fetch(&f->tfd->refcnt, 1);
    if (f->inet) inet_conn_addref(f->inet);
    if (f->node) node_ref(f->node);
    if (!f->pipe) return;
    if (f->wpipe) {
        f->pipe->read_refs++;
        f->wpipe->write_refs++;
        return;
    }
    if (f->pipe_end == PIPE_END_READ)
        f->pipe->read_refs++;
    else
        f->pipe->write_refs++;
}

void vfs_file_addref(vfs_file_t *f) { file_addref(f); }

vfs_file_t *vfs_file_clone(vfs_file_t *f) {
    if (!file_valid(f)) return NULL;
    vfs_file_t *copy = file_alloc();
    if (!copy) return NULL;
    *copy = *f;
    copy->magic = VFS_FILE_MAGIC;
    copy->cloexec = 0;
    file_addref(copy);
    return copy;
}

int vfs_fd_adopt(vfs_file_t *f) {
    if (!file_valid(f)) return -(int) EBADF;
    int fd = fd_alloc_from(0);
    if (fd < 0) return -(int) EMFILE;
    vfs_cur_fds()[fd] = f;
    return fd;
}

void vfs_set_fdtable(vfs_file_t **fds) {
    if (fds) g_cur_fds = fds;
}
vfs_file_t **vfs_get_fdtable(void) { return vfs_cur_fds(); }

static void wire_stdio(vfs_file_t **fds) {
    static const char *paths[] = { "/dev/stdin", "/dev/stdout", "/dev/stderr" };
    static const int flags[] = { O_RDONLY, O_WRONLY, O_WRONLY };
    for (int i = 0; i <= 2; i++) {
        vfs_node_t *n = vfs_lookup(paths[i]);
        if (!n) continue;
        vfs_file_t *f = file_alloc();
        if (!f) {
            node_unref(n);
            continue;
        }
        f->node = n;
        f->flags = flags[i];
        /* n is already reffed by vfs_lookup; that ref is now owned by the fd */
        fds[i] = f;
    }
}

void vfs_copy_fdtable(vfs_file_t **dst, vfs_file_t **src) {
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (!src[i]) {
            dst[i] = NULL;
            continue;
        }
        if (!file_valid(src[i])) {
            dst[i] = NULL;
            continue;
        }
        vfs_file_t *f = file_alloc();
        if (f) {
            *f = *src[i];
            f->magic = VFS_FILE_MAGIC;
            /* child doesnt own a listening sockets lifecycle */
            if (f->node && f->node->type == VFS_TYPE_SOCK) f->node = NULL;
            file_addref(f); /* bump node/pipe ref-counts */
        }
        dst[i] = f;
    }
}

int vfs_phantom_sanitize_fdtable(vfs_file_t **fds) {
    if (!fds) return -1;
    int sanitized = 0;
    for (int i = 3; i < VFS_FD_MAX; i++) {
        vfs_file_t *f = fds[i];
        if (!file_valid(f)) continue;
        if (f->inet) {
            net_conn_t *fake = inet_phantom_clone(f->inet);
            inet_conn_close(f->inet); /* drop only the child's inherited reference */
            f->inet = fake;
            if (fake) {
                sanitized++;
                continue;
            }
        }
        file_close(f);
        fds[i] = NULL;
        sanitized++;
    }
    return sanitized;
}

void vfs_free_fdtable(vfs_file_t **fds) {
    if (!fds) return;
    for (int i = 0; i < VFS_FD_MAX; i++) {
        if (fds[i]) {
            if (file_valid(fds[i])) file_close(fds[i]);
            fds[i] = NULL;
        }
    }
    kfree(fds);
}

/* close every FD_CLOEXEC fd in the current table; called on a successful execve */
void vfs_cloexec_flush(void) {
    vfs_file_t **fds = vfs_cur_fds();
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (fds[i] && file_valid(fds[i]) && fds[i]->cloexec) fd_close(i);
}

int vfs_register_fs(struct filesystem *fs) {
    if (!fs || g_filesystem_cnt >= FS_MAX) return -1;
    g_filesystems[g_filesystem_cnt++] = fs;
    return 0;
}

struct filesystem *vfs_find_fs(const char *name) {
    for (int i = 0; i < g_filesystem_cnt; i++)
        if (strcmp(g_filesystems[i]->name, name) == 0) return g_filesystems[i];
    return NULL;
}

int vfs_fs_count(void) { return g_filesystem_cnt; }

struct filesystem *vfs_get_fs(int i) {
    return (i >= 0 && i < g_filesystem_cnt) ? g_filesystems[i] : NULL;
}

void vfs_sync_all(void) {
    for (int i = 0; i < g_filesystem_cnt; i++)
        if (g_filesystems[i]->sync) g_filesystems[i]->sync();
}

void vfs_init(void) {
    g_root = node_alloc("/", VFS_TYPE_DIR, 0755 | S_IFDIR);
    g_root->parent = g_root;

    vfs_mkdir_p("/sys", 0555);
    vfs_mkdir_p("/tmp", 01777);
    vfs_mkdir_p("/tmp/.X11-unix", 01777);
    vfs_mkdir_p("/etc", 0755);
    vfs_mkdir_p("/var/log", 0755);
    vfs_mkdir_p("/var/lib/xkb", 0755);
    vfs_mkdir_p("/run", 0755);
    vfs_mkdir_p("/sys/class/graphics/fb0/device", 0555);
    vfs_mkdir_p("/sys/bus/platform", 0555);
    vfs_create_symlink("/sys/class/graphics/fb0/device/subsystem", "/sys/bus/platform");

    devfs_init();

    procfs_init();

    wire_stdio(g_default_fds);
    log_info("VFS:  root mounted  (ramfs)");
}

static void fill_stat(vfs_node_t *n, struct linux_stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = n->ino;
    st->st_nlink = 1;
    st->st_mode = n->mode;
    st->st_uid = n->uid;
    st->st_gid = n->gid;
    st->st_size = (int64_t) n->size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t) ((n->size + 511) / 512);
}

const char *vfs_copy_kernel_path(const char *path, char *kbuf) {
    if (!path) return NULL;
    size_t i = 0;
    for (; i < 511 && path[i]; i++) kbuf[i] = path[i];
    kbuf[i] = '\0';
    return kbuf;
}

const char *vfs_copy_user_path(const char *path, char *kbuf) {
    if (!path) return NULL;
    if ((uint64_t) (uintptr_t) path >= USER_LIMIT) return NULL;
    for (size_t i = 0; i < 511; i++) {
        const char *a = path + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return NULL;
        }
        char c = *a;
        kbuf[i] = c;
        if (!c) return kbuf;
    }
    const char *last = path + 511;
    if (!uptr_ok(last, 1) || *last) return NULL;
    kbuf[511] = '\0';
    return kbuf;
}

static int fd_open_impl(const char *path, int flags, int mode, bool reroot, bool from_user) {
    char _pbuf[512];
    path = from_user ? vfs_copy_user_path(path, _pbuf) : vfs_copy_kernel_path(path, _pbuf);
    if (!path) return -(int) EFAULT;

    /* /proc/self/fd/N and /dev/fd/N - dup the existing fd */
    const char *fd_prefix = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        fd_prefix = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        fd_prefix = path + 8;
    if (fd_prefix && *fd_prefix >= '0' && *fd_prefix <= '9') {
        int src = 0;
        for (const char *p = fd_prefix; *p >= '0' && *p <= '9'; p++) src = src * 10 + (*p - '0');
        return fd_dup(src);
    }

    /* re-root through the jail (if any) before resolving; host procs are unchanged */
    char abspath[512];
    const char *lpath = path;
    if (reroot) {
        vfs_abs_path(abspath, sizeof(abspath), path);
        if (abspath[0]) lpath = abspath;
    }

    vfs_node_t *n = (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(lpath) : vfs_lookup(lpath);
    bool from_lookup = (n != NULL);

    if (!n) {
        if (!(flags & O_CREAT)) return -(int) ENOENT;
        const char *leaf;
        vfs_node_t *parent = parent_of(lpath, &leaf);
        (void) leaf;
        if (!parent || !may_create_in(parent)) {
            node_unref(parent);
            return -(int) EACCES;
        }
        node_unref(parent);
        n = vfs_create_file(lpath, mode, NULL, 0);
        if (!n) return -(int) ENOMEM;
        for (int i = 0; i < g_filesystem_cnt; i++) {
            if (g_filesystems[i]->create && g_filesystems[i]->create(n, lpath, mode) == 0) break;
        }
        node_ref(n); /* ref for the fd; create_file returns unrefed node */
    } else {
        if ((flags & O_CREAT) && (flags & O_EXCL)) {
            node_unref(n);
            return -(int) EEXIST;
        }
    }
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_USE |
                                (((flags & O_ACCMODE) != O_RDONLY ||
                                  (flags & O_TRUNC))
                                     ? ANTI_TOCTOU_VFS_WRITE
                                     : 0u));
    if (n->type == VFS_TYPE_CHR && n->chr_open) {
        int r = n->chr_open(n, flags);
        node_unref(n); /* chr_open created its own fd with its own ref */
        return r;
    }

    /* a directory may be opened read-only (e.g. for fstat/getdents); only
     * reject it when write access is requested. */
    if (n->type == VFS_TYPE_DIR && (flags & O_ACCMODE) != O_RDONLY) {
        node_unref(n);
        return -(int) EISDIR;
    }

    {
        int acc = (flags & O_ACCMODE);
        uint32_t need = (acc != O_WRONLY ? 4u : 0u) | (acc != O_RDONLY ? 2u : 0u);
        if ((flags & O_TRUNC) && n->type == VFS_TYPE_REG) need |= 2u;
        if (!may_access(n, need)) {
            node_unref(n);
            return -(int) EACCES;
        }
    }

    int fd = fd_alloc_from(3);
    if (fd < 0) {
        node_unref(n);
        return -(int) EMFILE;
    }

    vfs_file_t *f = file_alloc();
    if (!f) {
        node_unref(n);
        return -(int) ENOMEM;
    }

    f->node = n;
    f->flags = flags;
    f->pos = 0;
    f->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    /* n is already reffed: from_lookup -> lookup_ref bumped it; !from_lookup -> node_ref above */
    (void) from_lookup;
    if ((flags & O_TRUNC) && n->type == VFS_TYPE_REG) n->size = 0;
    if (flags & O_APPEND) f->pos = n->size;

    vfs_cur_fds()[fd] = f;
    return fd;
}

int fd_open(const char *path, int flags, int mode) {
    return fd_open_impl(path, flags, mode, true, true);
}

/* kernel-internal: open a host-absolute path, bypassing jail re-rooting */
int fd_open_host(const char *path, int flags, int mode) {
    return fd_open_impl(path, flags, mode, false, false);
}

int fd_open_kpath(const char *path, int flags, int mode) {
    return fd_open_impl(path, flags, mode, true, false);
}

int fd_openat(int dirfd, const char *path, int flags, int mode) {
    char resolved[512];
    int rc = at_resolve(dirfd, path, resolved, sizeof(resolved));
    if (rc < 0) return rc;
    return fd_open_host(resolved, flags, mode);
}

int fd_close(int fd) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int) EBADF;
    file_close(f);
    vfs_cur_fds()[fd] = NULL;
    return 0;
}

int64_t fd_read(int fd, void *buf, uint64_t len) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (len == 0) return 0;
    if (!uptr_ok_w(buf, len)) /*  needs a writable page */
        return -(int64_t) EFAULT;
    if (f->efd) return eventfd_read(f, (char *) buf, len);
    if (f->tfd) return timerfd_read(f, (char *) buf, len);
    if (f->inet) return inet_fd_read(f->inet, buf, len, f->flags);

    if (!f->pipe && !f->wpipe && (f->flags & O_ACCMODE) == O_WRONLY) return -(int64_t) EBADF;

    if (f->wpipe) { /* socket */
        if ((f->flags & O_NONBLOCK) && f->pipe->count == 0 && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_read(f->pipe, buf, len);
    }

    /* pipe */
    if (f->pipe) {
        if (f->pipe_end != PIPE_END_READ) return -(int64_t) EBADF;
        if ((f->flags & O_NONBLOCK) && f->pipe->count == 0 && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_read(f->pipe, buf, len);
    }

    vfs_node_t *n = f->node;
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_USE);
    if (n->type == VFS_TYPE_CHR) {
        if (!n->chr_read) return 0;
        if ((f->flags & O_NONBLOCK) && n->chr_pollin && !n->chr_pollin(n)) return -(int64_t) EAGAIN;
        int64_t r = n->chr_read(n, (char *) buf, len, f->pos);
        if (r > 0) f->pos += (uint64_t) r;
        return r;
    }
    if (n->type == VFS_TYPE_DIR) return -(int64_t) EISDIR;
    if (n->type == VFS_TYPE_REG) {
        if (n->fs_ops && n->fs_ops->read) {
            int64_t r = n->fs_ops->read(n, (char *) buf, f->pos, len);
            if (r > 0) f->pos += (uint64_t) r;
            return r;
        }
        if (f->pos >= n->size) return 0;
        uint64_t avail = n->size - f->pos;
        uint64_t r = (len < avail) ? len : avail;
        memcpy(buf, n->data + f->pos, r);
        f->pos += r;
        return (int64_t) r;
    }
    return -(int64_t) EINVAL;
}

int64_t fd_peek(int fd, void *buf, uint64_t len, uint64_t skip) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (len == 0) return 0;
    if (!uptr_ok_w(buf, len)) return -(int64_t) EFAULT;

    if (f->wpipe) {
        if ((f->flags & O_NONBLOCK) && f->pipe->count <= skip && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_peek(f->pipe, buf, len, skip);
    }

    if (f->pipe) {
        if (f->pipe_end != PIPE_END_READ) return -(int64_t) EBADF;
        if ((f->flags & O_NONBLOCK) && f->pipe->count <= skip && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_peek(f->pipe, buf, len, skip);
    }

    return -(int64_t) EINVAL;
}

static int64_t fd_write_dispatch(vfs_file_t *f, const void *buf, uint64_t len) {
    if (f->inet) return inet_fd_write(f->inet, buf, len);

    if (!f->pipe && !f->wpipe && (f->flags & O_ACCMODE) == O_RDONLY) return -(int64_t) EBADF;

    if (f->wpipe) /* socket */
        return pipe_write(f->wpipe, buf, len);

    /* pipe */
    if (f->pipe) {
        if (f->pipe_end != PIPE_END_WRITE) return -(int64_t) EBADF;
        return pipe_write(f->pipe, buf, len);
    }

    vfs_node_t *n = f->node;
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_USE |
                                ANTI_TOCTOU_VFS_WRITE);
    if (n->type == VFS_TYPE_CHR) {
        if (!n->chr_write) return (int64_t) len;
        int64_t r = n->chr_write(n, (const char *) buf, len, f->pos);
        if (r > 0) f->pos += (uint64_t) r;
        return r;
    }
    if (n->type == VFS_TYPE_DIR) return -(int64_t) EISDIR;
    if (n->type == VFS_TYPE_REG) {
        if (n->fs_ops && n->fs_ops->write) {
            int64_t r = n->fs_ops->write(n, (const char *) buf, f->pos, len);
            if (r > 0) f->pos += (uint64_t) r;
            return r;
        }
        if (f->pos > INT64_MAX || len > (uint64_t) INT64_MAX - f->pos)
            return -(int64_t) EFBIG;
        uint64_t end = f->pos + len;
        if (end > n->capacity) {
            if (end > UINT64_MAX - 4095) return -(int64_t) EFBIG;
            uint64_t newcap = (end + 4095) & ~4095ULL;
            uint8_t *newdata = (uint8_t *) kmalloc(newcap);
            if (!newdata) return -(int64_t) ENOSPC;
            if (n->data) {
                memcpy(newdata, n->data, n->size);
                kfree(n->data);
            }
            n->data = newdata;
            n->capacity = newcap;
        }
        memcpy(n->data + f->pos, buf, len);
        f->pos += len;
        if (f->pos > n->size) n->size = f->pos;
        n->dirty = 1;
        return (int64_t) len;
    }
    return -(int64_t) EINVAL;
}

int64_t fd_write(int fd, const void *buf, uint64_t len) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (len == 0) return 0;
    if (!uptr_ok(buf, len)) return -(int64_t) EFAULT;
    if (f->efd) return eventfd_write(f, (const char *) buf, len);
    if (f->tfd) return -(int64_t) EINVAL; /* timerfd not writable via write() */
    if (f->wpipe && (f->flags & O_NONBLOCK) && f->wpipe->read_refs > 0) {
        uint64_t space = PIPE_BUFSZ - f->wpipe->count;
        if (space == 0) return -(int64_t) EAGAIN;
        if (len > space) len = space;
    }
    if (f->pipe && !f->wpipe && (f->flags & O_NONBLOCK) && f->pipe_end == PIPE_END_WRITE &&
        f->pipe->read_refs > 0) {
        uint64_t space = PIPE_BUFSZ - f->pipe->count;
        if (space == 0) return -(int64_t) EAGAIN;
        if (len > space) len = space;
    }
    return fd_write_dispatch(f, buf, len);
}

int64_t fd_write_kbuf(int fd, const void *buf, uint64_t len) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (len == 0) return 0;
    return fd_write_dispatch(f, buf, len);
}

int64_t fd_lseek(int fd, int64_t off, int whence) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int64_t) EBADF;
    if (f->pipe) return -(int64_t) ESPIPE;
    vfs_node_t *n = f->node;
    if (n->type == VFS_TYPE_CHR && n->size == 0) return -(int64_t) EINVAL;
    int64_t new_pos;
    switch (whence) {
    case SEEK_SET:
        new_pos = off;
        break;
    case SEEK_CUR:
        new_pos = (int64_t) f->pos + off;
        break;
    case SEEK_END:
        new_pos = (int64_t) n->size + off;
        break;
    default:
        return -(int64_t) EINVAL;
    }
    if (new_pos < 0) return -(int64_t) EINVAL;
    f->pos = (uint64_t) new_pos;
    return new_pos;
}

int fd_fstat(int fd, struct linux_stat *st) {
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int) EBADF;
    if (!st) return -(int) EINVAL;
    if (!uptr_ok_w(st, sizeof(*st))) return -(int) EFAULT;
    if (f->wpipe) { /* connected socket */
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFSOCK | 0666;
        st->st_blksize = PIPE_BUFSZ;
        return 0;
    }
    if (f->pipe) {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFIFO | 0600;
        st->st_blksize = PIPE_BUFSZ;
        return 0;
    }
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) f->node,
                            ANTI_TOCTOU_VFS_CHECK);
    fill_stat(f->node, st);
    return 0;
}

int fd_fstatat(int dirfd, const char *path, struct linux_stat *st, int flags) {
    char _pbuf[512];
    if (!path || !st) return -(int) EINVAL;
    if (!uptr_ok_w(st, sizeof(*st))) return -(int) EFAULT;
    const char *upath = path;
    if (!(path = vfs_copy_user_path(path, _pbuf))) return -(int) EFAULT;
    if (path[0] == '\0' && (flags & AT_EMPTY_PATH)) return fd_fstat(dirfd, st);
    char abspath[512];
    int rc = at_resolve(dirfd, upath, abspath, sizeof(abspath));
    if (rc < 0) return rc;
    vfs_node_t *n =
        (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(abspath) : vfs_lookup(abspath);
    if (!n) return -(int) ENOENT;
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_CHECK);
    fill_stat(n, st);
    node_unref(n);
    return 0;
}

int fd_stat(const char *path, struct linux_stat *st) {
    char _pbuf[512];
    if (!path || !st) return -(int) EINVAL;
    if (!uptr_ok_w(st, sizeof(*st))) return -(int) EFAULT;
    if (!(path = vfs_copy_user_path(path, _pbuf))) return -(int) EFAULT;
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -(int) ENOENT;
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_CHECK);
    fill_stat(n, st);
    node_unref(n);
    return 0;
}

int fd_lstat(const char *path, struct linux_stat *st) {
    char _pbuf[512];
    if (!path || !st) return -(int) EINVAL;
    if (!uptr_ok_w(st, sizeof(*st))) return -(int) EFAULT;
    if (!(path = vfs_copy_user_path(path, _pbuf))) return -(int) EFAULT;
    vfs_node_t *n = vfs_lookup_nofollow(path);
    if (!n) return -(int) ENOENT;
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_CHECK);
    fill_stat(n, st);
    node_unref(n);
    return 0;
}

int fd_getdents64(int fd, void *buf, uint64_t count) {
    if (!buf || !count) return -(int) EINVAL;
    if (!uptr_ok_w(buf, count)) return -(int) EFAULT;
    vfs_file_t *f = fd_get(fd);
    if (!f) return -(int) EBADF;
    if (f->pipe) return -(int) ENOTDIR;
    vfs_node_t *dir = f->node;
    if (dir->type != VFS_TYPE_DIR) return -(int) ENOTDIR;

    int proc_ret = 0;
    if (procfs_getdents64(dir, &f->pos, buf, count, &proc_ret)) return proc_ret;

    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;
    uint64_t idx = 0;
    uint64_t skip = f->pos;
    uint64_t emitted = 0;

    if (skip == 0) {
        const char *nm = ".";
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + 2 + 7) & ~7U);
        if (done + rec <= count) {
            struct linux_dirent64 *d = (struct linux_dirent64 *) (out + done);
            d->d_ino = dir->ino;
            d->d_off = 1;
            d->d_reclen = rec;
            d->d_type = DT_DIR;
            memcpy(d->d_name, nm, 2);
            done += rec;
            emitted++;
        }
    }
    idx = 1;
    if (skip <= 1) {
        const char *nm = "..";
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + 3 + 7) & ~7U);
        if (done + rec <= count) {
            struct linux_dirent64 *d = (struct linux_dirent64 *) (out + done);
            d->d_ino = dir->parent ? dir->parent->ino : dir->ino;
            d->d_off = 2;
            d->d_reclen = rec;
            d->d_type = DT_DIR;
            memcpy(d->d_name, nm, 3);
            done += rec;
            emitted++;
        }
    }
    idx = 2;

    uint64_t child_idx = 0;
    for (vfs_node_t *c = dir->children; c; c = c->next, child_idx++) {
        if (idx + child_idx < skip) continue;
        size_t nmlen = strlen(c->name) + 1;
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + nmlen + 7) & ~7U);
        if (done + rec > count) break;
        struct linux_dirent64 *d = (struct linux_dirent64 *) (out + done);
        d->d_ino = c->ino;
        d->d_off = (int64_t) (idx + child_idx + 1);
        d->d_reclen = rec;
        d->d_type = (c->type == VFS_TYPE_DIR) ? DT_DIR :
                    (c->type == VFS_TYPE_REG) ? DT_REG :
                    (c->type == VFS_TYPE_SYM) ? DT_LNK :
                    (c->type == VFS_TYPE_CHR) ? DT_CHR :
                                                DT_UNKNOWN;
        memcpy(d->d_name, c->name, nmlen);
        done += rec;
        emitted++;
    }

    if (emitted == 0 && done == 0) return 0;
    f->pos += emitted;
    return (int) done;
}

static int readlink_common(const char *path, char *buf, uint64_t bufsz) {
    int proc_ret = 0;
    if (procfs_readlink(path, buf, bufsz, &proc_ret)) return proc_ret;
    vfs_node_t *n = vfs_lookup_nofollow(path);
    if (!n) return -(int) ENOENT;
    if (n->type != VFS_TYPE_SYM) {
        node_unref(n);
        return -(int) EINVAL;
    }
    uint64_t len = strlen(n->symlink);
    if (len > bufsz) len = bufsz;
    memcpy(buf, n->symlink, len);
    node_unref(n);
    return (int) len;
}

int fd_readlink(const char *path, char *buf, uint64_t bufsz) {
    char _pbuf[512];
    if (!path || !buf || !bufsz) return -(int) EINVAL;
    if (!uptr_ok_w(buf, bufsz)) return -(int) EFAULT;
    if (!(path = vfs_copy_user_path(path, _pbuf))) return -(int) EFAULT;
    return readlink_common(path, buf, bufsz);
}

int fd_readlink_kpath(const char *path, char *buf, uint64_t bufsz) {
    if (!path || !buf || !bufsz) return -(int) EINVAL;
    if (!uptr_ok_w(buf, bufsz)) return -(int) EFAULT;
    return readlink_common(path, buf, bufsz);
}

char *vfs_node_abspath(vfs_node_t *n, char *buf, size_t sz) {
    if (!n || !buf || sz == 0) return NULL;
    if (n->parent == n) { /* root */
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }

    vfs_node_t *stack[128];
    int depth = 0;
    vfs_node_t *cur = n;
    while (cur && cur->parent != cur && depth < 128) {
        stack[depth++] = cur;
        cur = cur->parent;
    }

    char *p = buf;
    char *end = buf + sz - 1;
    for (int i = depth - 1; i >= 0; i--) {
        if (p < end) *p++ = '/';
        size_t nl = strlen(stack[i]->name);
        if (p + nl > end) nl = (size_t) (end - p);
        memcpy(p, stack[i]->name, nl);
        p += nl;
    }
    if (p == buf) *p++ = '/';
    *p = '\0';
    return buf;
}

int vfs_link(const char *oldpath, const char *newpath) {
    vfs_node_t *src = vfs_lookup(oldpath);
    if (!src) return -(int) ENOENT;
    if (src->type == VFS_TYPE_DIR) {
        node_unref(src);
        return -(int) EISDIR;
    }
    const char *leaf;
    vfs_node_t *parent = parent_of(newpath, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR) {
        node_unref(src);
        node_unref(parent);
        return -(int) ENOENT;
    }
    if (!leaf || !*leaf) {
        node_unref(src);
        node_unref(parent);
        return -(int) EINVAL;
    }
    if (!may_create_in(parent)) {
        node_unref(src);
        node_unref(parent);
        return -(int) EACCES;
    }
    vfs_node_t *ln = node_alloc(leaf, src->type, src->mode);
    if (!ln) {
        node_unref(src);
        node_unref(parent);
        return -(int) ENOMEM;
    }
    ln->size = src->size;
    ln->capacity = src->capacity;
    if (src->type == VFS_TYPE_REG && src->data && src->size > 0) {
        ln->data = (uint8_t *) kmalloc(src->size);
        if (!ln->data) {
            kfree(ln);
            node_unref(src);
            node_unref(parent);
            return -(int) ENOMEM;
        }
        memcpy(ln->data, src->data, src->size);
        ln->capacity = src->size;
    } else if (src->type == VFS_TYPE_SYM && src->symlink) {
        size_t len = strlen(src->symlink) + 1;
        ln->symlink = (char *) kmalloc(len);
        if (!ln->symlink) {
            kfree(ln);
            node_unref(src);
            node_unref(parent);
            return -(int) ENOMEM;
        }
        memcpy(ln->symlink, src->symlink, len);
        ln->size = len - 1;
        ln->capacity = 0;
    }
    uint64_t _f = irq_save();
    if (dir_find(parent, leaf)) {
        irq_restore(_f);
        kfree(ln);
        node_unref(src);
        node_unref(parent);
        return -(int) EEXIST;
    }
    dir_insert_nolock(parent, ln);
    irq_restore(_f);
    node_unref(src);
    node_unref(parent);
    return 0;
}

int vfs_chmod(const char *path, uint32_t mode) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -(int) ENOENT;
    if (!may_change_mode(n)) {
        node_unref(n);
        return -(int) EPERM;
    }
    n->mode = (n->mode & ~07777U) | mode_without_priv_bits(mode);
    node_unref(n);
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode) {
    vfs_node_t *n = fd_get_node(fd);
    if (!n) return -(int) EBADF;
    if (!may_change_mode(n)) return -(int) EPERM;
    n->mode = (n->mode & ~07777U) | mode_without_priv_bits(mode);
    return 0;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -(int) ENOENT;
    if (!may_change_owner(n)) {
        node_unref(n);
        return -(int) EPERM;
    }
    if (uid != (uint32_t) -1) n->uid = uid;
    if (gid != (uint32_t) -1) n->gid = gid;
    n->mode &= ~06000U;
    node_unref(n);
    return 0;
}

int vfs_lchown(const char *path, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = vfs_lookup_nofollow(path);
    if (!n) return -(int) ENOENT;
    if (!may_change_owner(n)) {
        node_unref(n);
        return -(int) EPERM;
    }
    if (uid != (uint32_t) -1) n->uid = uid;
    if (gid != (uint32_t) -1) n->gid = gid;
    n->mode &= ~06000U;
    node_unref(n);
    return 0;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid) {
    vfs_node_t *n = fd_get_node(fd);
    if (!n) return -(int) EBADF;
    if (!may_change_owner(n)) return -(int) EPERM;
    if (uid != (uint32_t) -1) n->uid = uid;
    if (gid != (uint32_t) -1) n->gid = gid;
    n->mode &= ~06000U;
    return 0;
}

int vfs_truncate(const char *path, uint64_t len) {
    vfs_node_t *n = vfs_lookup(path);
    if (!n) return -(int) ENOENT;
    if (n->type != VFS_TYPE_REG) {
        node_unref(n);
        return -(int) EINVAL;
    }
    if (!may_access(n, 2u)) {
        node_unref(n);
        return -(int) EACCES;
    }
    if (len < n->size) {
        n->size = len;
        n->dirty = 1;
    }
    node_unref(n);
    return 0;
}

int vfs_access(const char *path, int mode) {
    if (mode & ~7) return -(int) EINVAL;
    vfs_node_t *n = vfs_lookup(path);
    if (!n) {
        phantom_record(PHANTOM_EVENT_ACCESS, (uint32_t) mode, 0, 0, "VFS lookup denied");
        return -(int) ENOENT;
    }
    anti_toctou_observe_vfs((uint64_t) (uintptr_t) n,
                            ANTI_TOCTOU_VFS_CHECK);
    if (mode == 0) {
        node_unref(n);
        return 0;
    }
    if ((mode & 1) && !(n->mode & 0111U)) {
        phantom_record(PHANTOM_EVENT_ACCESS, (uint32_t) mode, n->ino, 0,
                       "VFS execute denied");
        node_unref(n);
        return -(int) EACCES;
    }
    int r = may_access(n, (uint32_t) mode) ? 0 : -(int) EACCES;
    if (r < 0)
        phantom_record(PHANTOM_EVENT_ACCESS, (uint32_t) mode, n->ino, 0,
                       "VFS permission denied");
    node_unref(n);
    return r;
}

int vfs_mknod(const char *path, uint32_t mode, uint64_t dev) {
    (void) dev;
    const char *leaf;
    vfs_node_t *parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR) {
        node_unref(parent);
        return -(int) ENOENT;
    }
    if (!leaf || !*leaf) {
        node_unref(parent);
        return -(int) EINVAL;
    }
    if (!cred_is_root()) {
        node_unref(parent);
        return -(int) EPERM;
    }
    if (!may_create_in(parent)) {
        node_unref(parent);
        return -(int) EACCES;
    }
    uint32_t ftype = mode & S_IFMT;
    if (!ftype) ftype = S_IFREG;
    uint8_t type = (ftype == S_IFDIR) ? VFS_TYPE_DIR : VFS_TYPE_REG;
    vfs_node_t *n = node_alloc(leaf, type, apply_umask(mode & 07777U) | ftype);
    if (!n) {
        node_unref(parent);
        return -(int) ENOMEM;
    }
    uint64_t _f = irq_save();
    if (dir_find(parent, leaf)) {
        irq_restore(_f);
        kfree(n);
        node_unref(parent);
        return -(int) EEXIST;
    }
    dir_insert_nolock(parent, n);
    irq_restore(_f);
    node_unref(parent);
    return 0;
}

int at_resolve(int dirfd, const char *path, char *out, size_t sz) {
    char _pbuf[512];
    if (sz) out[0] = '\0';
    if (!(path = vfs_copy_user_path(path, _pbuf))) return -(int) EFAULT;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        vfs_abs_path(out, sz, path);
        return 0;
    }
    vfs_file_t *df = fd_get(dirfd);
    if (!df || !df->node || df->node->type != VFS_TYPE_DIR) return -(int) EBADF;
    char dirpath[512];
    if (!vfs_node_abspath(df->node, dirpath, sizeof(dirpath))) return -(int) EINVAL;
    size_t dl = strlen(dirpath);
    if (dl + 1 + strlen(path) >= sz) return -(int) ENAMETOOLONG;
    memcpy(out, dirpath, dl);
    if (out[dl - 1] != '/') out[dl++] = '/';
    strcpy(out + dl, path);
    const char *root = jail_root_current(); /* dirpath already host-rooted; just clamp ".." */
    if (root[0]) jail_canon_clamp(out, sz, root);
    return 0;
}
