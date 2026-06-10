#include "vfs.h"
#include "arch/x86_64/cpu.h"
#include "drivers/fb.h"
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

static vfs_node_t* node_alloc(const char* name, uint8_t type, uint32_t mode)
{
    vfs_node_t* n = (vfs_node_t*) kcalloc(1, sizeof(vfs_node_t));
    if (!n)
        return NULL;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = type;
    n->mode = mode;
    n->ino = g_next_ino++;
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

static void file_close(vfs_file_t* f)
{
    if (!f)
        return;
    if (f->pipe)
    {
        if (f->pipe_end == PIPE_END_READ)
        {
            if (f->pipe->read_refs)
                f->pipe->read_refs--;
        }
        else
        {
            if (f->pipe->write_refs)
                f->pipe->write_refs--;
            /* wake a waiting reader when the last write end closes */
            if (f->pipe->write_refs == 0 && f->pipe->waiting_reader)
            {
                proc_t* reader = (proc_t*) f->pipe->waiting_reader;
                if (reader->state == PROC_WAITING)
                    reader->state = PROC_READY;
            }
        }
        if (f->pipe->read_refs == 0 && f->pipe->write_refs == 0)
            kfree(f->pipe);
    }
    kfree(f);
}

static void file_addref(vfs_file_t* f)
{
    if (!f || !f->pipe)
        return;
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

void vfs_init(void)
{
    g_root = node_alloc("/", VFS_TYPE_DIR, 0755 | S_IFDIR);
    g_root->parent = g_root;

    vfs_mkdir_p("/dev", 0755);
    vfs_mkdir_p("/proc", 0555);
    vfs_mkdir_p("/sys", 0555);
    vfs_mkdir_p("/tmp", 01777);
    vfs_mkdir_p("/etc", 0755);

    vfs_create_chr("/dev/null", dev_null_read, dev_null_write);
    vfs_create_chr("/dev/zero", dev_zero_read, dev_null_write);
    vfs_create_chr("/dev/tty", dev_tty_read, dev_tty_write);
    vfs_create_chr("/dev/stdin", dev_tty_read, dev_null_write);
    vfs_create_chr("/dev/stdout", dev_null_read, dev_tty_write);
    vfs_create_chr("/dev/stderr", dev_null_read, dev_tty_write);
    vfs_create_symlink("/dev/console", "/dev/tty");
    vfs_create_symlink("/dev/fd", "/proc/self/fd");

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

int fd_open(const char* path, int flags, int mode)
{
    if (!path)
        return -(int) ENOENT;

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

#define POLLIN 1
#define POLLOUT 4

bool fd_pollin(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return false;
    if (f->pipe) return true;
    if (f->node && f->node->type == VFS_TYPE_CHR)
    {
        tty_process_input();
        return tty_data_ready();
    }
    return true;
}

bool fd_pollout(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f) return false;
    return true;
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
        if (g_fb.addr)
        {
            ws->ws_col = (uint16_t) (g_fb.width / 8);
            ws->ws_row = (uint16_t) (g_fb.height / 16);
            ws->ws_xpixel = (uint16_t) g_fb.width;
            ws->ws_ypixel = (uint16_t) g_fb.height;
        }
        else
        {
            ws->ws_col = 80;
            ws->ws_row = 25;
            ws->ws_xpixel = 0;
            ws->ws_ypixel = 0;
        }
        return 0;
    }
    case TCGETS:
    {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (!t)
            return -(int) EINVAL;
        tty_get_termios((struct termios_s*) t);
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case 0x5404: /* TCSETSF */
    {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (t)
            tty_set_termios((const struct termios_s*) t);
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
