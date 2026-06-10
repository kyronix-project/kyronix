#include "vfs.h"
#include "syscall/syscall.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"
#include "drivers/tty.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"

static vfs_node_t* g_root = NULL;
static uint32_t g_next_ino = 1;

char g_cwd[512] = "/";

static vfs_file_t* g_default_fds[VFS_FD_MAX];
static vfs_file_t** g_fds = g_default_fds;

#define EACCES    13
#define EFAULT    14
#define EEXIST    17
#define ENOENT     2
#define EBADF      9
#define ENOMEM    12
#define ENOTDIR   20
#define EISDIR    21
#define EINVAL    22
#define EMFILE    24
#define ENOTTY    25
#define ENOSPC    28
#define ESPIPE    29
#define ENOTEMPTY 39
#define ENAMETOOLONG 36

static vfs_node_t* node_alloc(const char* name, uint8_t type, uint32_t mode)
{
    vfs_node_t* n = (vfs_node_t*) kcalloc(1, sizeof(vfs_node_t));
    if (!n)
        return NULL;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = type;
    n->mode = mode;
    n->ino  = g_next_ino++;
    if (g_current_proc) { n->uid = g_current_proc->euid; n->gid = g_current_proc->egid; }
    return n;
}

static void dir_insert(vfs_node_t* dir, vfs_node_t* child)
{
    child->parent = dir;
    child->next = dir->children;
    dir->children = child;
}

static vfs_node_t* dir_find(vfs_node_t* dir, const char* name)
{
    for (vfs_node_t* c = dir->children; c; c = c->next)
        if (strcmp(c->name, name) == 0)
            return c;
    return NULL;
}

static void dir_remove(vfs_node_t* parent, vfs_node_t* child)
{
    if (parent->children == child) {
        parent->children = child->next;
    } else {
        for (vfs_node_t* c = parent->children; c; c = c->next)
            if (c->next == child) { c->next = child->next; break; }
    }
    child->next = NULL;
    child->parent = NULL;
}

static void vfs_abs_path(char* out, size_t sz, const char* in)
{
    if (!in || in[0] == '/') {
        strncpy(out, in ? in : "", sz - 1);
        out[sz - 1] = '\0';
        return;
    }
    const char* cwd = (g_current_proc && g_current_proc->cwd[0]) ? g_current_proc->cwd : g_cwd;
    size_t cl = strlen(cwd);
    if (cl >= sz) { out[0] = '\0'; return; }
    memcpy(out, cwd, cl);
    if (out[cl - 1] != '/') out[cl++] = '/';
    strncpy(out + cl, in, sz - cl - 1);
    out[sz - 1] = '\0';
}

static vfs_node_t* lookup_internal(const char* path, bool follow_last, int depth)
{
    if (!path || path[0] == '\0')
        return NULL;
    if (depth > 32)
        return NULL;

    vfs_node_t* cur;
    if (path[0] == '/')
    {
        cur = g_root;
    }
    else
    {
        const char* cwd = (g_current_proc && g_current_proc->cwd[0]) ? g_current_proc->cwd : g_cwd;
        size_t cwd_len = strlen(cwd);
        char full[512];
        if (cwd_len + 1 + strlen(path) >= sizeof(full))
            return NULL;
        memcpy(full, cwd, cwd_len);
        if (full[cwd_len - 1] != '/')
            full[cwd_len++] = '/';
        strcpy(full + cwd_len, path);
        return lookup_internal(full, follow_last, depth + 1);
    }
    if (!cur)
        return NULL;

    const char* p = path;
    while (*p == '/')
        p++;
    if (*p == '\0')
        return cur;

    while (*p)
    {
        const char* start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t) (p - start);
        while (*p == '/')
            p++;

        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.')
        {
            if (cur->parent)
                cur = cur->parent;
            continue;
        }

        char comp[256];
        if (len >= sizeof(comp))
            return NULL;
        memcpy(comp, start, len);
        comp[len] = '\0';

        bool last = (*p == '\0');
        if (cur->type != VFS_TYPE_DIR)
            return NULL;
        vfs_node_t* child = dir_find(cur, comp);
        if (!child)
            return NULL;

        if (child->type == VFS_TYPE_SYM && (follow_last || !last))
        {
            if (!child->symlink)
                return NULL;
            child = lookup_internal(child->symlink, true, depth + 1);
            if (!child)
                return NULL;
        }
        cur = child;
    }
    return cur;
}

vfs_node_t* vfs_lookup(const char* path)
{
    return lookup_internal(path, true, 0);
}
vfs_node_t* vfs_lookup_nofollow(const char* path)
{
    return lookup_internal(path, false, 0);
}

vfs_node_t* vfs_mkdir_p(const char* path, uint32_t mode)
{
    if (!path || path[0] != '/')
        return NULL;
    vfs_node_t* cur = g_root;
    const char* p = path + 1;
    while (*p)
    {
        const char* start = p;
        while (*p && *p != '/')
            p++;
        size_t len = (size_t) (p - start);
        while (*p == '/')
            p++;
        if (len == 0)
            continue;
        char comp[256];
        if (len >= sizeof(comp))
            return NULL;
        memcpy(comp, start, len);
        comp[len] = '\0';
        vfs_node_t* child = dir_find(cur, comp);
        if (!child)
        {
            child = node_alloc(comp, VFS_TYPE_DIR, mode | S_IFDIR);
            if (!child)
                return NULL;
            dir_insert(cur, child);
        }
        if (child->type != VFS_TYPE_DIR)
            return NULL;
        cur = child;
    }
    return cur;
}

static vfs_node_t* parent_of(const char* path, const char** leaf)
{
    const char* slash = NULL;
    for (const char* p = path; *p; p++)
        if (*p == '/')
            slash = p;
    if (!slash || slash == path)
    {
        *leaf = path + (path[0] == '/' ? 1 : 0);
        return g_root;
    }
    size_t plen = (size_t) (slash - path);
    char parent_path[512];
    if (plen >= sizeof(parent_path))
        return NULL;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';
    *leaf = slash + 1;
    return vfs_lookup(plen ? parent_path : "/");
}

vfs_node_t* vfs_create_file(const char* path, uint32_t mode, const void* data, uint64_t size)
{
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR)
        return NULL;

    vfs_node_t* existing = dir_find(parent, leaf);
    if (existing)
    {
        if (existing->type != VFS_TYPE_REG)
            return NULL;
        kfree(existing->data);
        existing->data = NULL;
        existing->size = existing->capacity = 0;
        if (size > 0)
        {
            existing->data = (uint8_t*) kmalloc(size);
            if (!existing->data)
                return NULL;
            memcpy(existing->data, data, size);
            existing->size = existing->capacity = size;
        }
        return existing;
    }

    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_REG, (mode & 07777) | S_IFREG);
    if (!n)
        return NULL;
    if (size > 0)
    {
        n->data = (uint8_t*) kmalloc(size);
        if (!n->data)
        {
            kfree(n);
            return NULL;
        }
        memcpy(n->data, data, size);
        n->size = n->capacity = size;
    }
    dir_insert(parent, n);
    return n;
}

vfs_node_t* vfs_create_symlink(const char* path, const char* target)
{
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent)
        return NULL;
    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_SYM, 0777 | S_IFLNK);
    if (!n)
        return NULL;
    n->symlink = (char*) kmalloc(strlen(target) + 1);
    if (!n->symlink)
    {
        kfree(n);
        return NULL;
    }
    strcpy(n->symlink, target);
    n->size = strlen(target);
    dir_insert(parent, n);
    return n;
}

vfs_node_t* vfs_create_chr(const char* path, int64_t (*rfn)(vfs_node_t*, char*, uint64_t, uint64_t),
                           int64_t (*wfn)(vfs_node_t*, const char*, uint64_t))
{
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent)
        return NULL;
    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_CHR, 0666 | S_IFCHR);
    if (!n)
        return NULL;
    n->chr_read = rfn;
    n->chr_write = wfn;
    dir_insert(parent, n);
    return n;
}

int vfs_mkdir(const char* path, uint32_t mode)
{
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR || !leaf || !*leaf) return -(int)ENOENT;
    if (dir_find(parent, leaf)) return -(int)EEXIST;
    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_DIR, (mode & 07777) | S_IFDIR);
    if (!n) return -(int)ENOMEM;
    dir_insert(parent, n);
    return 0;
}

int vfs_unlink(const char* path)
{
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n) return -(int)ENOENT;
    if (n->type == VFS_TYPE_DIR) return -(int)EISDIR;
    if (!n->parent) return -(int)EINVAL;
    dir_remove(n->parent, n);
    if (n->data) kfree(n->data);
    if (n->symlink) kfree(n->symlink);
    kfree(n);
    return 0;
}

int vfs_rmdir(const char* path)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n) return -(int)ENOENT;
    if (n->type != VFS_TYPE_DIR) return -(int)ENOTDIR;
    if (n->children) return -(int)ENOTEMPTY;
    if (!n->parent) return -(int)EINVAL;
    dir_remove(n->parent, n);
    kfree(n);
    return 0;
}

int vfs_rename(const char* oldpath, const char* newpath)
{
    vfs_node_t* n = vfs_lookup_nofollow(oldpath);
    if (!n || !n->parent) return -(int)ENOENT;
    const char* new_leaf;
    vfs_node_t* new_parent = parent_of(newpath, &new_leaf);
    if (!new_parent || new_parent->type != VFS_TYPE_DIR) return -(int)ENOENT;
    if (!new_leaf || !*new_leaf) return -(int)EINVAL;
    vfs_node_t* existing = dir_find(new_parent, new_leaf);
    if (existing) {
        if (existing->type == VFS_TYPE_DIR) {
            if (n->type != VFS_TYPE_DIR) return -(int)EISDIR;
            if (existing->children) return -(int)ENOTEMPTY;
        } else if (n->type == VFS_TYPE_DIR) {
            return -(int)ENOTDIR;
        }
        dir_remove(new_parent, existing);
        if (existing->data) kfree(existing->data);
        if (existing->symlink) kfree(existing->symlink);
        kfree(existing);
    }
    dir_remove(n->parent, n);
    strncpy(n->name, new_leaf, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    dir_insert(new_parent, n);
    return 0;
}

static int64_t dev_null_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}
static int64_t dev_null_write(vfs_node_t* n, const char* buf, uint64_t len)
{
    (void) n;
    (void) buf;
    return (int64_t) len;
}
static int64_t dev_zero_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) off;
    memset(buf, 0, len);
    return (int64_t) len;
}
static int64_t dev_urandom_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n; (void) off;
    static uint64_t s = 0xdeadbeef13579aceULL;
    uint8_t* p = (uint8_t*) buf;
    for (uint64_t i = 0; i < len; i++)
    {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        p[i] = (uint8_t) s;
    }
    return (int64_t) len;
}
static int64_t dev_tty_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) off;
    return tty_read(buf, len);
}
static int64_t dev_tty_write(vfs_node_t* n, const char* buf, uint64_t len)
{
    (void) n;
    return tty_write(buf, len);
}

static int fd_alloc_from(int start)
{
    for (int i = start; i < VFS_FD_MAX; i++)
        if (!g_fds[i])
            return i;
    return -1;
}

static vfs_file_t* fd_get(int fd)
{
    if (fd < 0 || fd >= VFS_FD_MAX)
        return NULL;
    return g_fds[fd];
}

bool fd_valid(int fd)
{
    return fd_get(fd) != NULL;
}

vfs_node_t* fd_get_node(int fd)
{
    vfs_file_t* f = fd_get(fd);
    return f ? f->node : NULL;
}

vfs_file_t* fd_get_file(int fd)
{
    return fd_get(fd);
}

int64_t fd_pread(int fd, void* buf, uint64_t len, uint64_t off)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return -(int64_t)EBADF;
    if (f->pipe) return -(int64_t)ESPIPE;
    if (len == 0) return 0;
    if (!uptr_ok(buf, len)) return -(int64_t)EFAULT;
    vfs_node_t* n = f->node;
    if (!n || n->type != VFS_TYPE_REG) return -(int64_t)EINVAL;
    if (off >= n->size) return 0;
    uint64_t avail = n->size - off;
    uint64_t r = (len < avail) ? len : avail;
    memcpy(buf, n->data + off, r);
    return (int64_t)r;
}

int64_t fd_pwrite(int fd, const void* buf, uint64_t len, uint64_t off)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return -(int64_t)EBADF;
    if (f->pipe) return -(int64_t)ESPIPE;
    if (len == 0) return 0;
    if (!uptr_ok(buf, len)) return -(int64_t)EFAULT;
    vfs_node_t* n = f->node;
    if (!n || n->type != VFS_TYPE_REG) return -(int64_t)EINVAL;
    uint64_t end = off + len;
    if (end > n->capacity) {
        uint64_t newcap = (end + 4095) & ~4095ULL;
        uint8_t* newdata = (uint8_t*)kmalloc(newcap);
        if (!newdata) return -(int64_t)ENOSPC;
        if (n->data) { memcpy(newdata, n->data, n->size); kfree(n->data); }
        n->data = newdata; n->capacity = newcap;
    }
    memcpy(n->data + off, buf, len);
    if (end > n->size) n->size = end;
    return (int64_t)len;
}

bool fd_pollin(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return false;
    if (f->wpipe) /* socket: readable when read-pipe has data */
        return f->pipe->count > 0 || f->pipe->write_refs == 0;
    if (f->pipe)
        return f->pipe_end == PIPE_END_READ &&
               (f->pipe->count > 0 || f->pipe->write_refs == 0);
    if (!f->node) return false;
    if (f->node->type == VFS_TYPE_CHR)
        return tty_data_ready();
    return true;
}

bool fd_pollout(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return false;
    if (f->wpipe) /* socket: writable when write-pipe has space */
        return f->wpipe->count < PIPE_BUFSZ && f->wpipe->read_refs > 0;
    if (f->pipe)
        return f->pipe_end == PIPE_END_WRITE &&
               f->pipe->count < PIPE_BUFSZ && f->pipe->read_refs > 0;
    return f->node != NULL;
}

static void pipe_drop_write(pipe_t* p)
{
    if (!p) return;
    if (p->write_refs) p->write_refs--;
    if (p->write_refs == 0 && p->waiting_reader)
    {
        proc_t* reader = (proc_t*) p->waiting_reader;
        if (reader->state == PROC_WAITING)
            reader->state = PROC_READY;
    }
}

static void pipe_maybe_free(pipe_t* p)
{
    if (p && p->read_refs == 0 && p->write_refs == 0)
        kfree(p);
}

static void file_close(vfs_file_t* f)
{
    if (!f)
        return;
    if (f->wpipe)
    {
        /* socket fd: f->pipe is the read pipe, f->wpipe is the write pipe */
        if (f->pipe->read_refs) f->pipe->read_refs--;
        pipe_maybe_free(f->pipe);
        pipe_drop_write(f->wpipe);
        pipe_maybe_free(f->wpipe);
    }
    else if (f->pipe)
    {
        if (f->pipe_end == PIPE_END_READ)
        {
            if (f->pipe->read_refs)
                f->pipe->read_refs--;
        }
        else
        {
            pipe_drop_write(f->pipe);
        }
        pipe_maybe_free(f->pipe);
    }
    kfree(f);
}

static void file_addref(vfs_file_t* f)
{
    if (!f || !f->pipe)
        return;
    if (f->wpipe)
    {
        f->pipe->read_refs++;
        f->wpipe->write_refs++;
        return;
    }
    if (f->pipe_end == PIPE_END_READ)
        f->pipe->read_refs++;
    else
        f->pipe->write_refs++;
}

void vfs_set_fdtable(vfs_file_t** fds)
{
    g_fds = fds;
}

vfs_file_t** vfs_get_fdtable(void)
{
    return g_fds;
}

static void wire_stdio(vfs_file_t** fds)
{
    static const char* paths[] = {"/dev/stdin", "/dev/stdout", "/dev/stderr"};
    static const int flags[] = {O_RDONLY, O_WRONLY, O_WRONLY};
    for (int i = 0; i <= 2; i++)
    {
        vfs_node_t* n = vfs_lookup(paths[i]);
        if (!n)
            continue;
        vfs_file_t* f = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
        if (!f)
            continue;
        f->node = n;
        f->flags = flags[i];
        fds[i] = f;
    }
}

void vfs_copy_fdtable(vfs_file_t** dst, vfs_file_t** src)
{
    for (int i = 0; i < VFS_FD_MAX; i++)
    {
        if (!src[i])
        {
            dst[i] = NULL;
            continue;
        }
        vfs_file_t* f = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
        if (f)
        {
            *f = *src[i];
            file_addref(f); /* bump pipe ref-counts */
        }
        dst[i] = f;
    }
}

void vfs_free_fdtable(vfs_file_t** fds)
{
    if (!fds)
        return;
    for (int i = 0; i < VFS_FD_MAX; i++)
    {
        if (fds[i])
        {
            file_close(fds[i]);
            fds[i] = NULL;
        }
    }
    kfree(fds);
}

static int64_t proc_version_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char ver[] = "Kyronix version 0.1 (x86_64)\n";
    uint64_t sz = sizeof(ver) - 1;
    if (off >= sz) return 0;
    uint64_t r = sz - off < len ? sz - off : len;
    memcpy(buf, ver + off, r);
    return (int64_t) r;
}

static int64_t proc_empty_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n; (void) buf; (void) len; (void) off;
    return 0;
}

static int64_t proc_exe_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    if (!g_current_proc || !g_current_proc->exe_path[0]) return 0;
    const char* p = g_current_proc->exe_path;
    uint64_t sz = strlen(p);
    if (off >= sz) return 0;
    uint64_t r = sz - off < len ? sz - off : len;
    memcpy(buf, p + off, r);
    return (int64_t) r;
}

void vfs_init(void)
{
    g_root = node_alloc("/", VFS_TYPE_DIR, 0755 | S_IFDIR);
    g_root->parent = g_root;

    vfs_mkdir_p("/dev", 0755);
    vfs_mkdir_p("/proc", 0555);
    vfs_mkdir_p("/proc/self", 0555);
    vfs_mkdir_p("/proc/self/fd", 0500);
    vfs_mkdir_p("/sys", 0555);
    vfs_mkdir_p("/tmp", 01777);
    vfs_mkdir_p("/etc", 0755);

    vfs_create_chr("/dev/null",    dev_null_read,    dev_null_write);
    vfs_create_chr("/dev/zero",    dev_zero_read,    dev_null_write);
    vfs_create_chr("/dev/urandom", dev_urandom_read, dev_null_write);
    vfs_create_chr("/dev/random",  dev_urandom_read, dev_null_write);
    vfs_create_chr("/dev/tty", dev_tty_read, dev_tty_write);
    vfs_create_chr("/dev/stdin", dev_tty_read, dev_null_write);
    vfs_create_chr("/dev/stdout", dev_null_read, dev_tty_write);
    vfs_create_chr("/dev/stderr", dev_null_read, dev_tty_write);
    vfs_create_symlink("/dev/console", "/dev/tty");
    vfs_create_symlink("/dev/fd", "/proc/self/fd");

    vfs_create_chr("/proc/version", proc_version_read, dev_null_write);
    vfs_create_chr("/proc/self/exe", proc_exe_read, dev_null_write);
    vfs_create_chr("/proc/self/maps", proc_empty_read, dev_null_write);
    vfs_create_chr("/proc/self/cmdline", proc_empty_read, dev_null_write);

    wire_stdio(g_default_fds);
    log_info("VFS:  root mounted  (ramfs)");
}

static void fill_stat(vfs_node_t* n, struct linux_stat* st)
{
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


int fd_pipe(int pipefd[2])
{
    pipe_t* p = pipe_alloc();
    if (!p)
        return -(int) ENOMEM;
    p->read_refs = 1;
    p->write_refs = 1;

    int rfd = fd_alloc_from(0);
    if (rfd < 0)
    {
        pipe_free(p);
        return -(int) EMFILE;
    }

    vfs_file_t* rf = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (!rf)
    {
        pipe_free(p);
        return -(int) ENOMEM;
    }
    rf->pipe = p;
    rf->pipe_end = PIPE_END_READ;
    rf->flags = O_RDONLY;
    g_fds[rfd] = rf;

    int wfd = fd_alloc_from(0);
    if (wfd < 0)
    {
        kfree(rf);
        g_fds[rfd] = NULL;
        pipe_free(p);
        return -(int) EMFILE;
    }

    vfs_file_t* wf = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (!wf)
    {
        kfree(rf);
        g_fds[rfd] = NULL;
        pipe_free(p);
        return -(int) ENOMEM;
    }
    wf->pipe = p;
    wf->pipe_end = PIPE_END_WRITE;
    wf->flags = O_WRONLY;
    g_fds[wfd] = wf;

    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

int fd_socketpair(int sv[2])
{
    /* two cross-connected pipes: sv[0] reads pipe_a, writes pipe_b; sv[1] vice versa */
    pipe_t* pa = pipe_alloc();
    pipe_t* pb = pipe_alloc();
    if (!pa || !pb) { kfree(pa); kfree(pb); return -(int) ENOMEM; }

    pa->read_refs = 1; pa->write_refs = 1;
    pb->read_refs = 1; pb->write_refs = 1;

    int fd0 = fd_alloc_from(0);
    int fd1 = (fd0 >= 0) ? fd_alloc_from(0) : -1;
    if (fd0 < 0 || fd1 < 0)
    {
        if (fd0 >= 0) g_fds[fd0] = NULL;
        kfree(pa); kfree(pb);
        return -(int) EMFILE;
    }

    vfs_file_t* f0 = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    vfs_file_t* f1 = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (!f0 || !f1) { kfree(f0); kfree(f1); kfree(pa); kfree(pb); return -(int) ENOMEM; }

    f0->pipe = pa; f0->wpipe = pb; f0->pipe_end = PIPE_END_READ; f0->flags = O_RDWR;
    f1->pipe = pb; f1->wpipe = pa; f1->pipe_end = PIPE_END_READ; f1->flags = O_RDWR;

    g_fds[fd0] = f0;
    g_fds[fd1] = f1;
    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

int fd_open(const char* path, int flags, int mode)
{
    if (!path)
        return -(int) ENOENT;

    /* /proc/self/fd/N and /dev/fd/N — dup the existing fd */
    const char* fd_prefix = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        fd_prefix = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        fd_prefix = path + 8;
    if (fd_prefix && *fd_prefix >= '0' && *fd_prefix <= '9')
    {
        int src = 0;
        for (const char* p = fd_prefix; *p >= '0' && *p <= '9'; p++)
            src = src * 10 + (*p - '0');
        return fd_dup(src);
    }

    vfs_node_t* n = (flags & O_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);

    if (!n)
    {
        if (!(flags & O_CREAT))
            return -(int) ENOENT;
        char abspath[512];
        vfs_abs_path(abspath, sizeof(abspath), path);
        n = vfs_create_file(abspath[0] ? abspath : path, mode, NULL, 0);
        if (!n)
            return -(int) ENOMEM;
    }
    else
    {
        if ((flags & O_CREAT) && (flags & O_EXCL))
            return -(int) EEXIST;
    }
    if (n->type == VFS_TYPE_DIR && !(flags & O_DIRECTORY))
        return -(int) EISDIR;

    {
        uint32_t uid = g_current_proc ? g_current_proc->euid : 0;
        if (uid != 0 && n->type == VFS_TYPE_REG)
        {
            int acc = (flags & O_ACCMODE);
            uint32_t need = (acc != O_WRONLY ? 4u : 0u) | (acc != O_RDONLY ? 2u : 0u);
            uint32_t bits = (n->uid == uid) ? ((n->mode >> 6) & 7u) : (n->mode & 7u);
            if ((bits & need) != need)
                return -(int) EACCES;
        }
    }

    int fd = fd_alloc_from(3);
    if (fd < 0)
        return -(int) EMFILE;

    vfs_file_t* f = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (!f)
        return -(int) ENOMEM;

    f->node = n;
    f->flags = flags;
    f->pos = 0;
    if (flags & O_TRUNC)
        n->size = 0;
    if (flags & O_APPEND)
        f->pos = n->size;

    g_fds[fd] = f;
    return fd;
}

int fd_openat(int dirfd, const char* path, int flags, int mode)
{
    if (!path)
        return -(int) ENOENT;
    if (path[0] == '/' || dirfd == AT_FDCWD)
        return fd_open(path, flags, mode);
    vfs_file_t* df = fd_get(dirfd);
    if (!df || !df->node || df->node->type != VFS_TYPE_DIR)
        return -(int) EBADF;
    return fd_open(path, flags, mode);
}

int fd_close(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    file_close(f);
    g_fds[fd] = NULL;
    return 0;
}

int64_t fd_read(int fd, void* buf, uint64_t len)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (len == 0)
        return 0;
    if (!uptr_ok(buf, len))
        return -(int64_t) EFAULT;

    if (f->wpipe) /* socket */
        return pipe_read(f->pipe, buf, len);

    /* Pipe */
    if (f->pipe)
    {
        if (f->pipe_end != PIPE_END_READ)
            return -(int64_t) EBADF;
        return pipe_read(f->pipe, buf, len);
    }

    vfs_node_t* n = f->node;
    if (n->type == VFS_TYPE_CHR)
    {
        if (!n->chr_read)
            return 0;
        return n->chr_read(n, (char*) buf, len, f->pos);
    }
    if (n->type == VFS_TYPE_DIR)
        return -(int64_t) EISDIR;
    if (n->type == VFS_TYPE_REG)
    {
        if (f->pos >= n->size)
            return 0;
        uint64_t avail = n->size - f->pos;
        uint64_t r = (len < avail) ? len : avail;
        memcpy(buf, n->data + f->pos, r);
        f->pos += r;
        return (int64_t) r;
    }
    return -(int64_t) EINVAL;
}

int64_t fd_write(int fd, const void* buf, uint64_t len)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (len == 0)
        return 0;
    if (!uptr_ok(buf, len))
        return -(int64_t) EFAULT;

    if (f->wpipe) /* socket */
        return pipe_write(f->wpipe, buf, len);

    /* Pipe */
    if (f->pipe)
    {
        if (f->pipe_end != PIPE_END_WRITE)
            return -(int64_t) EBADF;
        return pipe_write(f->pipe, buf, len);
    }

    vfs_node_t* n = f->node;
    if (n->type == VFS_TYPE_CHR)
    {
        if (!n->chr_write)
            return (int64_t) len;
        return n->chr_write(n, (const char*) buf, len);
    }
    if (n->type == VFS_TYPE_DIR)
        return -(int64_t) EISDIR;
    if (n->type == VFS_TYPE_REG)
    {
        uint64_t end = f->pos + len;
        if (end > n->capacity)
        {
            uint64_t newcap = (end + 4095) & ~4095ULL;
            uint8_t* newdata = (uint8_t*) kmalloc(newcap);
            if (!newdata)
                return -(int64_t) ENOSPC;
            if (n->data)
            {
                memcpy(newdata, n->data, n->size);
                kfree(n->data);
            }
            n->data = newdata;
            n->capacity = newcap;
        }
        memcpy(n->data + f->pos, buf, len);
        f->pos += len;
        if (f->pos > n->size)
            n->size = f->pos;
        return (int64_t) len;
    }
    return -(int64_t) EINVAL;
}

int64_t fd_lseek(int fd, int64_t off, int whence)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (f->pipe)
        return -(int64_t) ESPIPE;
    vfs_node_t* n = f->node;
    if (n->type == VFS_TYPE_CHR)
        return -(int64_t) EINVAL;
    int64_t new_pos;
    switch (whence)
    {
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
    if (new_pos < 0)
        return -(int64_t) EINVAL;
    f->pos = (uint64_t) new_pos;
    return new_pos;
}

int fd_fstat(int fd, struct linux_stat* st)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    if (!st)
        return -(int) EINVAL;
    if (f->pipe)
    {
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFIFO | 0600;
        st->st_blksize = PIPE_BUFSZ;
        return 0;
    }
    fill_stat(f->node, st);
    return 0;
}

int fd_fstatat(int dirfd, const char* path, struct linux_stat* st, int flags)
{
    if (!path || !st)
        return -(int) EINVAL;
    if (path[0] == '\0' && (flags & AT_EMPTY_PATH))
        return fd_fstat(dirfd, st);
    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        vfs_node_t* n =
            (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
        if (!n)
            return -(int) ENOENT;
        fill_stat(n, st);
        return 0;
    }
    vfs_file_t* df = fd_get(dirfd);
    if (!df || !df->node || df->node->type != VFS_TYPE_DIR)
        return -(int) EBADF;
    vfs_node_t* n = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lookup_nofollow(path) : vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    fill_stat(n, st);
    return 0;
}

int fd_stat(const char* path, struct linux_stat* st)
{
    if (!path || !st)
        return -(int) EINVAL;
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    fill_stat(n, st);
    return 0;
}

int fd_lstat(const char* path, struct linux_stat* st)
{
    if (!path || !st)
        return -(int) EINVAL;
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n)
        return -(int) ENOENT;
    fill_stat(n, st);
    return 0;
}

int fd_getdents64(int fd, void* buf, uint64_t count)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    if (f->pipe)
        return -(int) ENOTDIR;
    vfs_node_t* dir = f->node;
    if (dir->type != VFS_TYPE_DIR)
        return -(int) ENOTDIR;

    uint8_t* out = (uint8_t*) buf;
    uint64_t done = 0;
    uint64_t idx = 0;
    uint64_t skip = f->pos;
    uint64_t emitted = 0;

    if (skip == 0)
    {
        const char* nm = ".";
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + 2 + 7) & ~7U);
        if (done + rec <= count)
        {
            struct linux_dirent64* d = (struct linux_dirent64*) (out + done);
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
    if (skip <= 1)
    {
        const char* nm = "..";
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + 3 + 7) & ~7U);
        if (done + rec <= count)
        {
            struct linux_dirent64* d = (struct linux_dirent64*) (out + done);
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
    for (vfs_node_t* c = dir->children; c; c = c->next, child_idx++)
    {
        if (idx + child_idx < skip)
            continue;
        size_t nmlen = strlen(c->name) + 1;
        uint16_t rec = (uint16_t) ((sizeof(struct linux_dirent64) + nmlen + 7) & ~7U);
        if (done + rec > count)
            break;
        struct linux_dirent64* d = (struct linux_dirent64*) (out + done);
        d->d_ino = c->ino;
        d->d_off = (int64_t) (idx + child_idx + 1);
        d->d_reclen = rec;
        d->d_type = (c->type == VFS_TYPE_DIR)   ? DT_DIR
                    : (c->type == VFS_TYPE_REG) ? DT_REG
                    : (c->type == VFS_TYPE_SYM) ? DT_LNK
                    : (c->type == VFS_TYPE_CHR) ? DT_CHR
                                                : DT_UNKNOWN;
        memcpy(d->d_name, c->name, nmlen);
        done += rec;
        emitted++;
    }

    if (emitted == 0 && done == 0)
        return 0;
    f->pos += emitted;
    return (int) done;
}

int fd_readlink(const char* path, char* buf, uint64_t bufsz)
{
    if (!path || !buf || !bufsz)
        return -(int) EINVAL;
    if (strcmp(path, "/proc/self/exe") == 0)
    {
        if (!g_current_proc || !g_current_proc->exe_path[0])
            return -(int) ENOENT;
        size_t n = strlen(g_current_proc->exe_path);
        if (n >= bufsz)
            n = bufsz - 1;
        memcpy(buf, g_current_proc->exe_path, n);
        buf[n] = '\0';
        return (int) n;
    }
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n)
        return -(int) ENOENT;
    if (n->type != VFS_TYPE_SYM)
        return -(int) EINVAL;
    uint64_t len = strlen(n->symlink);
    if (len > bufsz)
        len = bufsz;
    memcpy(buf, n->symlink, len);
    return (int) len;
}

#define TCGETS 0x5401
#define TCSETS 0x5402
#define TCSETSW 0x5403
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410

struct winsize
{
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};
struct termios
{
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_cc[19];
};

int fd_ioctl(int fd, uint64_t req, uint64_t arg)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    if (f->pipe)
        return -(int) ENOTTY;

    switch (req)
    {
    case TIOCGWINSZ:
    {
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws)
            return -(int) EINVAL;
        ws->ws_row = 25;
        ws->ws_col = 80;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TCGETS:
    {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (!t)
            return -(int) EINVAL;
        memset(t, 0, sizeof(*t));
        t->c_iflag = 0x500;
        t->c_oflag = 0x5;
        t->c_cflag = 0xBF;
        t->c_lflag = tty_get_lflag();
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case 0x5404: /* TCSETSF */
    {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (t)
            tty_set_lflag(t->c_lflag);
        return 0;
    }
    case 0x5405: /* TCGETA  */
    case 0x5406: /* TCSETA  */
    case 0x540B: /* TIOCSCTTY */
    case 0x5422: /* TIOCNOTTY */
        return 0;
    case TIOCGPGRP:
    {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid)
            *pgid = tty_get_fg_pgid();
        return 0;
    }
    case TIOCSPGRP:
    {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid)
            tty_set_fg_pgid(*pgid);
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
#define FD_CLOEXEC 1

int fd_fcntl(int fd, int cmd, uint64_t arg)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    switch (cmd)
    {
    case F_GETFD:
        return 0;
    case F_SETFD:
        return 0;
    case F_GETFL:
        return f->flags;
    case F_SETFL:
        f->flags = (int) arg;
        return 0;
    case F_DUPFD:
    {
        int newfd = fd_alloc_from((int) arg);
        if (newfd < 0)
            return -(int) EMFILE;
        vfs_file_t* nf = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
        if (!nf)
            return -(int) ENOMEM;
        *nf = *f;
        file_addref(nf);
        g_fds[newfd] = nf;
        return newfd;
    }
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
    vfs_file_t* f = fd_get(oldfd);
    if (!f)
        return -(int) EBADF;
    if (newfd < 0 || newfd >= VFS_FD_MAX)
        return -(int) EBADF;
    if (g_fds[newfd])
    {
        file_close(g_fds[newfd]);
        g_fds[newfd] = NULL;
    }
    vfs_file_t* nf = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (!nf)
        return -(int) ENOMEM;
    *nf = *f;
    file_addref(nf);
    g_fds[newfd] = nf;
    return newfd;
}

int fd_dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd) return -(int)EINVAL;
    int r = fd_dup2(oldfd, newfd);
    if (r >= 0 && (flags & O_CLOEXEC)) { /* cloexec not enforced yet, accepted */ }
    return r;
}

/* Reconstruct absolute path of node by walking parent pointers. */
char* vfs_node_abspath(vfs_node_t* n, char* buf, size_t sz)
{
    if (!n || !buf || sz == 0) return NULL;
    if (n->parent == n) { /* root */ buf[0] = '/'; buf[1] = '\0'; return buf; }

    /* collect ancestors */
    vfs_node_t* stack[128];
    int depth = 0;
    vfs_node_t* cur = n;
    while (cur && cur->parent != cur && depth < 128) {
        stack[depth++] = cur;
        cur = cur->parent;
    }

    char* p = buf;
    char* end = buf + sz - 1;
    for (int i = depth - 1; i >= 0; i--) {
        if (p < end) *p++ = '/';
        size_t nl = strlen(stack[i]->name);
        if (p + nl > end) nl = (size_t)(end - p);
        memcpy(p, stack[i]->name, nl);
        p += nl;
    }
    if (p == buf) *p++ = '/';
    *p = '\0';
    return buf;
}

int vfs_link(const char* oldpath, const char* newpath)
{
    vfs_node_t* src = vfs_lookup(oldpath);
    if (!src) return -(int)ENOENT;
    if (src->type == VFS_TYPE_DIR) return -(int)EISDIR;
    const char* leaf;
    vfs_node_t* parent = parent_of(newpath, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR) return -(int)ENOENT;
    if (!leaf || !*leaf) return -(int)EINVAL;
    if (dir_find(parent, leaf)) return -(int)EEXIST;
    /* hard link: create new node sharing same data buffer — shallow copy */
    vfs_node_t* ln = node_alloc(leaf, src->type, src->mode);
    if (!ln) return -(int)ENOMEM;
    ln->data = src->data;   /* shared reference (no refcount — simple impl) */
    ln->size = src->size;
    ln->capacity = src->capacity;
    dir_insert(parent, ln);
    return 0;
}

int vfs_chmod(const char* path, uint32_t mode)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n) return -(int)ENOENT;
    n->mode = (n->mode & ~07777U) | (mode & 07777U);
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n) return -(int)EBADF;
    n->mode = (n->mode & ~07777U) | (mode & 07777U);
    return 0;
}

int vfs_chown(const char* path, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n) return -(int)ENOENT;
    if (uid != (uint32_t)-1) n->uid = uid;
    if (gid != (uint32_t)-1) n->gid = gid;
    return 0;
}

int vfs_lchown(const char* path, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n) return -(int)ENOENT;
    if (uid != (uint32_t)-1) n->uid = uid;
    if (gid != (uint32_t)-1) n->gid = gid;
    return 0;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n) return -(int)EBADF;
    if (uid != (uint32_t)-1) n->uid = uid;
    if (gid != (uint32_t)-1) n->gid = gid;
    return 0;
}

int vfs_mknod(const char* path, uint32_t mode, uint64_t dev)
{
    (void)dev;
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR) return -(int)ENOENT;
    if (!leaf || !*leaf) return -(int)EINVAL;
    if (dir_find(parent, leaf)) return -(int)EEXIST;
    uint8_t type = ((mode & S_IFMT) == S_IFDIR) ? VFS_TYPE_DIR : VFS_TYPE_REG;
    vfs_node_t* n = node_alloc(leaf, type, mode);
    if (!n) return -(int)ENOMEM;
    dir_insert(parent, n);
    return 0;
}

/* Resolve *at dirfd+path into an absolute path stored in out[sz]. */
int at_resolve(int dirfd, const char* path, char* out, size_t sz)
{
    if (!path) return -(int)EFAULT;
    if (path[0] == '/' || dirfd == AT_FDCWD) {
        vfs_abs_path(out, sz, path);
        return 0;
    }
    vfs_file_t* df = fd_get(dirfd);
    if (!df || !df->node || df->node->type != VFS_TYPE_DIR) return -(int)EBADF;
    char dirpath[512];
    if (!vfs_node_abspath(df->node, dirpath, sizeof(dirpath))) return -(int)EINVAL;
    size_t dl = strlen(dirpath);
    if (dl + 1 + strlen(path) >= sz) return -(int)ENAMETOOLONG;
    memcpy(out, dirpath, dl);
    if (out[dl-1] != '/') out[dl++] = '/';
    strcpy(out + dl, path);
    return 0;
}
