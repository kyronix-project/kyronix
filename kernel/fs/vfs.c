#include "vfs.h"
#include "arch/x86_64/cpu.h"
#include "drivers/serial.h"
#include "drivers/tty.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/proc.h"
#include "syscall/syscall.h"

static int procfs_getdents64(vfs_node_t* dir, uint64_t* pos, void* buf, uint64_t count, int* ret);
static int procfs_readlink(const char* path, char* buf, uint64_t bufsz, int* ret);

static vfs_node_t* g_root = NULL;
static uint32_t g_next_ino = 1;

char g_cwd[512] = "/";

static vfs_file_t* g_default_fds[VFS_FD_MAX];
static vfs_file_t** g_fds = g_default_fds;

#define VFS_FILE_MAGIC 0x4b59464d41474943ULL /* "KYFMAGIC" */

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

/* unix socket structs (used by file_close and fd_*_unix functions below) */
#define SOCK_UNBOUND 0
#define SOCK_BOUND 1
#define SOCK_LISTENING 2

/* abstract Unix socket registry (Linux @-namespace, sun_path[0]=='\0') */
#define MAX_ABSTRACT_SOCKS 16
static struct
{
    char name[107];
    vfs_node_t* node;
} g_abstract_socks[MAX_ABSTRACT_SOCKS];

typedef struct unix_conn
{
    pipe_t* cli_rx;
    pipe_t* srv_rx;
    uint32_t peer_pid;
    uint32_t peer_uid;
    uint32_t peer_gid;
    struct unix_conn* next;
} unix_conn_t;

typedef struct
{
    int state;
    char path[108];
    unix_conn_t* backlog;
    proc_t* accept_waiter;
} unix_sock_t;

static vfs_node_t* node_alloc(const char* name, uint8_t type, uint32_t mode)
{
    vfs_node_t* n = (vfs_node_t*) kcalloc(1, sizeof(vfs_node_t));
    if (!n)
        return NULL;
    strncpy(n->name, name, sizeof(n->name) - 1);
    n->type = type;
    n->mode = mode;
    n->ino = g_next_ino++;
    if (g_current_proc)
    {
        n->uid = g_current_proc->fsuid;
        n->gid = g_current_proc->fsgid;
    }
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
    if (parent->children == child)
    {
        parent->children = child->next;
    }
    else
    {
        for (vfs_node_t* c = parent->children; c; c = c->next)
            if (c->next == child)
            {
                c->next = child->next;
                break;
            }
    }
    child->next = NULL;
    child->parent = NULL;
}

static uint32_t cred_fsuid(void)
{
    return g_current_proc ? g_current_proc->fsuid : 0;
}

static uint32_t cred_fsgid(void)
{
    return g_current_proc ? g_current_proc->fsgid : 0;
}

static bool cred_is_root(void)
{
    return !g_current_proc || g_current_proc->euid == 0;
}

static bool cred_fsroot(void)
{
    return !g_current_proc || g_current_proc->fsuid == 0;
}

static bool may_access(vfs_node_t* n, uint32_t need)
{
    if (!n)
        return false;
    if (cred_fsroot())
        return true;
    uint32_t bits;
    if (n->uid == cred_fsuid())
        bits = (n->mode >> 6) & 7u;
    else if (n->gid == cred_fsgid())
        bits = (n->mode >> 3) & 7u;
    else
        bits = n->mode & 7u;
    return (bits & need) == need;
}

static bool may_create_in(vfs_node_t* dir)
{
    return dir && dir->type == VFS_TYPE_DIR && may_access(dir, 3u);
}

static uint32_t mode_without_priv_bits(uint32_t mode)
{
    return cred_is_root() ? (mode & 07777U) : (mode & 01777U);
}

static uint32_t apply_umask(uint32_t mode)
{
    return g_current_proc ? (mode & ~g_current_proc->umask) : mode;
}

static bool may_change_owner(vfs_node_t* n)
{
    (void) n;
    return cred_is_root();
}

static bool may_change_mode(vfs_node_t* n)
{
    return n && (cred_is_root() || n->uid == cred_fsuid());
}

static void vfs_abs_path(char* out, size_t sz, const char* in)
{
    if (!in || in[0] == '/')
    {
        strncpy(out, in ? in : "", sz - 1);
        out[sz - 1] = '\0';
        return;
    }
    const char* cwd = (g_current_proc && g_current_proc->cwd[0]) ? g_current_proc->cwd : g_cwd;
    size_t cl = strlen(cwd);
    if (cl >= sz)
    {
        out[0] = '\0';
        return;
    }
    memcpy(out, cwd, cl);
    if (out[cl - 1] != '/')
        out[cl++] = '/';
    strncpy(out + cl, in, sz - cl - 1);
    out[sz - 1] = '\0';
}

static int vfs_node_path(vfs_node_t* n, char* buf, size_t sz)
{
    if (!n || !buf || sz == 0)
        return -1;
    if (n->parent == n)
    {
        if (sz < 2)
            return -1;
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    vfs_node_t* stack[128];
    int depth = 0;
    for (vfs_node_t* cur = n; cur && cur->parent != cur && depth < 128; cur = cur->parent)
        stack[depth++] = cur;
    if (depth >= 128)
        return -1;

    size_t pos = 0;
    buf[pos++] = '/';
    for (int i = depth - 1; i >= 0; i--)
    {
        if (pos > 1)
            buf[pos++] = '/';
        size_t len = strlen(stack[i]->name);
        if (pos + len >= sz)
            return -1;
        memcpy(buf + pos, stack[i]->name, len);
        pos += len;
    }
    buf[pos] = '\0';
    return 0;
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
        if (!may_access(cur, 1u))
            return NULL;
        vfs_node_t* child = dir_find(cur, comp);
        if (!child)
            return NULL;

        if (child->type == VFS_TYPE_SYM && (follow_last || !last))
        {
            if (!child->symlink)
                return NULL;

            char resolved[512];
            if (child->symlink[0] == '/')
            {
                if (*p)
                    snprintf(resolved, sizeof(resolved), "%s/%s", child->symlink, p);
                else
                    snprintf(resolved, sizeof(resolved), "%s", child->symlink);
            }
            else
            {
                char base[512];
                if (vfs_node_path(child->parent, base, sizeof(base)) < 0)
                    return NULL;
                if (strcmp(base, "/") == 0)
                {
                    if (*p)
                        snprintf(resolved, sizeof(resolved), "/%s/%s", child->symlink, p);
                    else
                        snprintf(resolved, sizeof(resolved), "/%s", child->symlink);
                }
                else
                {
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

    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_REG, apply_umask(mode & 07777) | S_IFREG);
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
    if (!may_create_in(parent))
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
    if (!parent || parent->type != VFS_TYPE_DIR || !leaf || !*leaf)
        return -(int) ENOENT;
    if (!may_create_in(parent))
        return -(int) EACCES;
    if (dir_find(parent, leaf))
        return -(int) EEXIST;
    vfs_node_t* n = node_alloc(leaf, VFS_TYPE_DIR, apply_umask(mode & 07777) | S_IFDIR);
    if (!n)
        return -(int) ENOMEM;
    dir_insert(parent, n);
    return 0;
}

int vfs_unlink(const char* path)
{
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n)
        return -(int) ENOENT;
    if (n->type == VFS_TYPE_DIR)
        return -(int) EISDIR;
    if (!n->parent)
        return -(int) EINVAL;
    if (!may_create_in(n->parent))
        return -(int) EACCES;
    dir_remove(n->parent, n);
    if (n->type != VFS_TYPE_SOCK && n->data)
        kfree(n->data); /* sock data owned by the fd */
    if (n->symlink)
        kfree(n->symlink);
    kfree(n);
    return 0;
}

int vfs_rmdir(const char* path)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    if (n->type != VFS_TYPE_DIR)
        return -(int) ENOTDIR;
    if (n->children)
        return -(int) ENOTEMPTY;
    if (!n->parent)
        return -(int) EINVAL;
    if (!may_create_in(n->parent))
        return -(int) EACCES;
    dir_remove(n->parent, n);
    kfree(n);
    return 0;
}

int vfs_rename(const char* oldpath, const char* newpath)
{
    vfs_node_t* n = vfs_lookup_nofollow(oldpath);
    if (!n || !n->parent)
        return -(int) ENOENT;
    const char* new_leaf;
    vfs_node_t* new_parent = parent_of(newpath, &new_leaf);
    if (!new_parent || new_parent->type != VFS_TYPE_DIR)
        return -(int) ENOENT;
    if (!new_leaf || !*new_leaf)
        return -(int) EINVAL;
    if (!may_create_in(n->parent) || !may_create_in(new_parent))
        return -(int) EACCES;
    vfs_node_t* existing = dir_find(new_parent, new_leaf);
    if (existing)
    {
        if (existing->type == VFS_TYPE_DIR)
        {
            if (n->type != VFS_TYPE_DIR)
                return -(int) EISDIR;
            if (existing->children)
                return -(int) ENOTEMPTY;
        }
        else if (n->type == VFS_TYPE_DIR)
        {
            return -(int) ENOTDIR;
        }
        dir_remove(new_parent, existing);
        if (existing->data)
            kfree(existing->data);
        if (existing->symlink)
            kfree(existing->symlink);
        kfree(existing);
    }
    dir_remove(n->parent, n);
    strncpy(n->name, new_leaf, sizeof(n->name) - 1);
    n->name[sizeof(n->name) - 1] = '\0';
    dir_insert(new_parent, n);
    return 0;
}

static int64_t devmem_mmap(vfs_node_t* n, uint64_t off, uint64_t len, uint64_t va, uint64_t vflags)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (!p || !p->space)
        return -22; /* EINVAL */
    if (p->euid != 0)
        return -(int64_t) EPERM;
    off &= ~0xFFFULL; /* page-align physical address */
    uint64_t flags = vflags | VMM_PRESENT | VMM_USER | VMM_WRITE;
    for (uint64_t o = 0; o < len; o += 0x1000)
        vmm_map(p->space, va + o, off + o, flags);
    return (int64_t) va;
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
    (void) n;
    (void) off;
    static uint64_t s = 0xdeadbeef13579aceULL;
    uint8_t* p = (uint8_t*) buf;
    for (uint64_t i = 0; i < len; i++)
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
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

struct winsize
{
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

static pipe_t* g_pty_m2s;
static pipe_t* g_pty_s2m;
static struct winsize g_pty_winsize = {25, 80, 0, 0};
static int g_pty_slave_pgid = 0;

static int64_t pty_master_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) off;
    return g_pty_s2m ? pipe_read(g_pty_s2m, buf, len) : -(int64_t) EINVAL;
}

static int64_t pty_master_write(vfs_node_t* n, const char* buf, uint64_t len)
{
    (void) n;
    return g_pty_m2s ? pipe_write(g_pty_m2s, buf, len) : -(int64_t) EINVAL;
}

static int64_t pty_slave_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) off;
    return g_pty_m2s ? pipe_read(g_pty_m2s, buf, len) : -(int64_t) EINVAL;
}

static int64_t pty_slave_write(vfs_node_t* n, const char* buf, uint64_t len)
{
    (void) n;
    if (!g_pty_s2m)
        return -(int64_t) EINVAL;
    int64_t done = 0;
    for (uint64_t i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
        { /* ONLCR: \n -> \r\n toward xterm */
            static const char cr = '\r';
            pipe_write(g_pty_s2m, &cr, 1);
        }
        pipe_write(g_pty_s2m, buf + i, 1);
        done++;
    }
    return done;
}

static bool pty_master_pollin(vfs_node_t* n)
{
    (void) n;
    return g_pty_s2m && g_pty_s2m->count > 0;
}

static bool pty_slave_pollin(vfs_node_t* n)
{
    (void) n;
    return g_pty_m2s && g_pty_m2s->count > 0;
}

static int64_t pty_master_ioctl(vfs_node_t* n, uint64_t req, uint64_t arg)
{
    (void) n;
    switch (req)
    {
    case 0x80045430: /* TIOCGPTN */
        if (arg && !uptr_ok_w((void*) (uintptr_t) arg, sizeof(int)))
            return -EFAULT;
        if (arg)
            *(int*) (uintptr_t) arg = 0;
        return 0;
    case 0x40045431: /* TIOCSPTLCK */
    case 0x80045439: /* TIOCGPTLCK */
    case 0x80045432: /* TIOCGDEV */
        if (arg && req != 0x40045431 && !uptr_ok_w((void*) (uintptr_t) arg, sizeof(int)))
            return -EFAULT;
        if (arg && req != 0x40045431)
            *(int*) (uintptr_t) arg = 0;
        return 0;
    case 0x5413:
    { /* TIOCGWINSZ */
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws || !uptr_ok_w(ws, sizeof(*ws)))
            return -EFAULT;
        *ws = g_pty_winsize;
        return 0;
    }
    case 0x5414:
    { /* TIOCSWINSZ */
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws || !uptr_ok(ws, sizeof(*ws)))
            return -EFAULT;
        g_pty_winsize = *ws;
        /* send SIGWINCH to slave's foreground process group */
        for (int i = 0; i < PROC_MAX; i++)
            if (g_proctable[i].state != PROC_UNUSED && g_proctable[i].pgid == g_pty_slave_pgid)
                proc_send_signal(&g_proctable[i], SIGWINCH);
        return 0;
    }
    }
    return 0;
}

static int fd_alloc_from(int start)
{
    for (int i = start; i < VFS_FD_MAX; i++)
        if (!g_fds[i])
            return i;
    return -1;
}

static vfs_file_t* file_alloc(void)
{
    vfs_file_t* f = (vfs_file_t*) kcalloc(1, sizeof(vfs_file_t));
    if (f)
        f->magic = VFS_FILE_MAGIC;
    return f;
}

static bool file_valid(vfs_file_t* f)
{
    uintptr_t addr = (uintptr_t) f;
    if (!f || addr < HEAP_START || addr >= HEAP_MAX || (addr & 7))
        return false;
    return f->magic == VFS_FILE_MAGIC;
}

static vfs_file_t* fd_get(int fd)
{
    if (fd < 0 || fd >= VFS_FD_MAX)
        return NULL;
    vfs_file_t* f = g_fds[fd];
    if (!file_valid(f))
        return NULL;
    return f;
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
    if (!f)
        return -(int64_t) EBADF;
    if (f->pipe)
        return -(int64_t) ESPIPE;
    if (len == 0)
        return 0;
    if (!uptr_ok_w(buf, len))
        return -(int64_t) EFAULT; /* kernel writes into buf */
    vfs_node_t* n = f->node;
    if (!n || n->type != VFS_TYPE_REG)
        return -(int64_t) EINVAL;
    if (off >= n->size)
        return 0;
    uint64_t avail = n->size - off;
    uint64_t r = (len < avail) ? len : avail;
    memcpy(buf, n->data + off, r);
    return (int64_t) r;
}

int64_t fd_pwrite(int fd, const void* buf, uint64_t len, uint64_t off)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (f->pipe)
        return -(int64_t) ESPIPE;
    if (len == 0)
        return 0;
    if (!uptr_ok(buf, len))
        return -(int64_t) EFAULT;
    vfs_node_t* n = f->node;
    if (!n || n->type != VFS_TYPE_REG)
        return -(int64_t) EINVAL;
    uint64_t end = off + len;
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
    memcpy(n->data + off, buf, len);
    if (end > n->size)
        n->size = end;
    return (int64_t) len;
}

bool fd_pollin(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return false;
    if (f->wpipe) /* socket: readable when read-pipe has data */
        return f->pipe->count > 0 || f->pipe->write_refs == 0;
    if (f->pipe)
        return f->pipe_end == PIPE_END_READ && (f->pipe->count > 0 || f->pipe->write_refs == 0);
    if (!f->node)
        return false;
    if (f->node->type == VFS_TYPE_SOCK)
        return f->node->sock_backlog > 0;
    if (f->node->type == VFS_TYPE_CHR)
    {
        if (f->node->chr_pollin)
            return f->node->chr_pollin(f->node);
        return tty_data_ready();
    }
    return true;
}

bool fd_pollout(int fd)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return false;
    if (f->wpipe) /* socket: writable when write-pipe has space */
        return f->wpipe->count < PIPE_BUFSZ && f->wpipe->read_refs > 0;
    if (f->node && f->node->type == VFS_TYPE_SOCK)
        return false;
    if (f->pipe)
        return f->pipe_end == PIPE_END_WRITE && f->pipe->count < PIPE_BUFSZ &&
               f->pipe->read_refs > 0;
    return f->node != NULL;
}

static void pipe_drop_write(pipe_t* p)
{
    if (!p)
        return;
    if (p->write_refs)
        p->write_refs--;
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
        pipe_free(p);
}

static void file_close(vfs_file_t* f)
{
    if (!file_valid(f))
        return;
    /* listening/bound socket: free unix_sock_t and remove VFS bind node */
    if (!f->pipe && !f->wpipe && f->node && f->node->type == VFS_TYPE_SOCK)
    {
        unix_sock_t* s = (unix_sock_t*) f->node->data;
        if (s)
        {
            if (s->path[0])
            {
                vfs_unlink(s->path); /* remove bind node (doesn't free s) */
            }
            else if (s->path[1])
            {
                /* abstract socket: remove from global table */
                for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++)
                {
                    if (g_abstract_socks[i].node == f->node)
                    {
                        g_abstract_socks[i].node = NULL;
                        break;
                    }
                }
            }
            unix_conn_t* c = s->backlog;
            while (c)
            { /* drain pending connections */
                unix_conn_t* nx = c->next;
                pipe_drop_write(c->cli_rx);
                pipe_maybe_free(c->cli_rx);
                if (c->srv_rx->read_refs)
                    c->srv_rx->read_refs--;
                pipe_maybe_free(c->srv_rx);
                kfree(c);
                c = nx;
            }
            kfree(s);
        }
        kfree(f->node);
        f->magic = 0;
        kfree(f);
        return;
    }
    if (f->wpipe)
    {
        /* socket fd: f->pipe is the read pipe, f->wpipe is the write pipe */
        if (f->pipe->read_refs)
            f->pipe->read_refs--;
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
    f->magic = 0;
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
        vfs_file_t* f = file_alloc();
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
        if (!file_valid(src[i]))
        {
            dst[i] = NULL;
            continue;
        }
        vfs_file_t* f = file_alloc();
        if (f)
        {
            *f = *src[i];
            f->magic = VFS_FILE_MAGIC;
            file_addref(f); /* bump pipe ref-counts */
            /* child doesn't own a listening socket's lifecycle */
            if (f->node && f->node->type == VFS_TYPE_SOCK)
                f->node = NULL;
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
            if (file_valid(fds[i]))
                file_close(fds[i]);
            fds[i] = NULL;
        }
    }
    kfree(fds);
}

/* close every FD_CLOEXEC fd in the current table; called on a successful execve */
void vfs_cloexec_flush(void)
{
    for (int i = 0; i < VFS_FD_MAX; i++)
        if (g_fds[i] && file_valid(g_fds[i]) && g_fds[i]->cloexec)
            fd_close(i);
}

static int64_t proc_version_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    static const char ver[] = "Kyronix version 0.1 (x86_64)\n";
    uint64_t sz = sizeof(ver) - 1;
    if (off >= sz)
        return 0;
    uint64_t r = sz - off < len ? sz - off : len;
    memcpy(buf, ver + off, r);
    return (int64_t) r;
}

static int64_t proc_empty_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    (void) buf;
    (void) len;
    (void) off;
    return 0;
}

static int64_t proc_pagemap_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    proc_t* p = g_current_proc;
    if (p && p->euid != 0)
        return -(int64_t) EPERM;
    if (!p || !p->space || len < 8)
        return 0;
    uint64_t nentries = len / 8;
    uint64_t written = 0;
    for (uint64_t i = 0; i < nentries; i++)
    {
        uint64_t va = ((off / 8) + i) << 12;
        uint64_t phys = vmm_virt_to_phys(p->space, va);
        uint64_t entry = phys ? ((phys >> 12) | (1ULL << 63)) : 0;
        __builtin_memcpy(buf + i * 8, &entry, 8);
        written += 8;
    }
    return (int64_t) written;
}

static int64_t proc_exe_read(vfs_node_t* n, char* buf, uint64_t len, uint64_t off)
{
    (void) n;
    if (!g_current_proc || !g_current_proc->exe_path[0])
        return 0;
    const char* p = g_current_proc->exe_path;
    uint64_t sz = strlen(p);
    if (off >= sz)
        return 0;
    uint64_t r = sz - off < len ? sz - off : len;
    memcpy(buf, p + off, r);
    return (int64_t) r;
}

void vfs_init(void)
{
    g_root = node_alloc("/", VFS_TYPE_DIR, 0755 | S_IFDIR);
    g_root->parent = g_root;

    vfs_mkdir_p("/dev", 0755);
    vfs_mkdir_p("/dev/input", 0755);
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

    vfs_create_chr("/dev/null", dev_null_read, dev_null_write);
    vfs_create_chr("/dev/zero", dev_zero_read, dev_null_write);
    vfs_create_chr("/dev/urandom", dev_urandom_read, dev_null_write);
    vfs_create_chr("/dev/random", dev_urandom_read, dev_null_write);
    {
        vfs_node_t* mn = vfs_create_chr("/dev/mem", dev_null_read, dev_null_write);
        if (mn)
        {
            mn->mode = S_IFCHR | 0600;
            mn->chr_mmap = devmem_mmap;
        }
    }
    vfs_mkdir_p("/dev/pts", 0755);
    g_pty_m2s = pipe_alloc();
    g_pty_s2m = pipe_alloc();
    if (g_pty_m2s && g_pty_s2m)
    {
        g_pty_m2s->read_refs = g_pty_m2s->write_refs = 1;
        g_pty_s2m->read_refs = g_pty_s2m->write_refs = 1;
        vfs_node_t* ptmx = vfs_create_chr("/dev/ptmx", pty_master_read, pty_master_write);
        if (ptmx)
        {
            ptmx->chr_ioctl = pty_master_ioctl;
            ptmx->chr_pollin = pty_master_pollin;
        }
        vfs_node_t* pts0 = vfs_create_chr("/dev/pts/0", pty_slave_read, pty_slave_write);
        if (pts0)
            pts0->chr_pollin = pty_slave_pollin;
    }
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
    {
        vfs_node_t* pm = vfs_create_chr("/proc/self/pagemap", proc_pagemap_read, dev_null_write);
        if (pm)
            pm->mode = S_IFCHR | 0400;
    }

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

    vfs_file_t* rf = file_alloc();
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
        file_close(rf);
        g_fds[rfd] = NULL;
        pipe_free(p);
        return -(int) EMFILE;
    }

    vfs_file_t* wf = file_alloc();
    if (!wf)
    {
        file_close(rf);
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
    if (!pa || !pb)
    {
        pipe_free(pa);
        pipe_free(pb);
        return -(int) ENOMEM;
    }

    pa->read_refs = 1;
    pa->write_refs = 1;
    pb->read_refs = 1;
    pb->write_refs = 1;

    int fd0 = fd_alloc_from(0);
    if (fd0 < 0)
    {
        pipe_free(pa);
        pipe_free(pb);
        return -(int) EMFILE;
    }

    vfs_file_t* f0 = file_alloc();
    vfs_file_t* f1 = file_alloc();
    if (!f0 || !f1)
    {
        if (f0)
        {
            f0->magic = 0;
            kfree(f0);
        }
        if (f1)
        {
            f1->magic = 0;
            kfree(f1);
        }
        pipe_free(pa);
        pipe_free(pb);
        return -(int) ENOMEM;
    }

    f0->pipe = pa;
    f0->wpipe = pb;
    f0->pipe_end = PIPE_END_READ;
    f0->flags = O_RDWR;
    f1->pipe = pb;
    f1->wpipe = pa;
    f1->pipe_end = PIPE_END_READ;
    f1->flags = O_RDWR;

    g_fds[fd0] = f0;
    int fd1 = fd_alloc_from(0);
    if (fd1 < 0)
    {
        g_fds[fd0] = NULL;
        file_close(f0);
        file_close(f1);
        return -(int) EMFILE;
    }
    g_fds[fd1] = f1;
    sv[0] = fd0;
    sv[1] = fd1;
    return 0;
}

int fd_socket(int domain, int type, int proto)
{
    (void) proto;
    if (domain != 1 || (type & 0xf) != 1)
        return -(int) EINVAL; /* AF_UNIX SOCK_STREAM only */
    unix_sock_t* s = (unix_sock_t*) kcalloc(1, sizeof(unix_sock_t));
    if (!s)
        return -(int) ENOMEM;
    vfs_node_t* n = (vfs_node_t*) kcalloc(1, sizeof(vfs_node_t));
    if (!n)
    {
        kfree(s);
        return -(int) ENOMEM;
    }
    n->type = VFS_TYPE_SOCK;
    n->mode = S_IFSOCK | 0666;
    n->ino = g_next_ino++;
    n->data = (uint8_t*) s;
    int fd = fd_alloc_from(0);
    if (fd < 0)
    {
        kfree(s);
        kfree(n);
        return -(int) EMFILE;
    }
    vfs_file_t* f = file_alloc();
    if (!f)
    {
        kfree(s);
        kfree(n);
        return -(int) ENOMEM;
    }
    f->node = n;
    f->flags = O_RDWR | (type & O_NONBLOCK);
    f->cloexec = (type & O_CLOEXEC) ? 1 : 0; /* SOCK_CLOEXEC */
    g_fds[fd] = f;
    return fd;
}

int fd_bind_unix(int fd, const char* path)
{
    vfs_file_t* f = fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;
    unix_sock_t* s = (unix_sock_t*) f->node->data;
    if (s->state != SOCK_UNBOUND)
        return -(int) EINVAL;

    if (path[0] == '\0')
    {
        /* abstract socket: register in global table, no VFS node created */
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++)
        {
            if (!g_abstract_socks[i].node)
            {
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
        if (*p == '/')
        {
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
    if (!may_create_in(parent))
        return -(int) EACCES;
    if (dir_find(parent, leaf))
        return -(int) EEXIST;
    vfs_node_t* bn = node_alloc(leaf, VFS_TYPE_SOCK, S_IFSOCK | 0666);
    if (!bn)
        return -(int) ENOMEM;
    bn->data = (uint8_t*) s; /* same unix_sock_t shared with fd's node */
    dir_insert(parent, bn);
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->state = SOCK_BOUND;
    return 0;
}

int fd_listen_unix(int fd, int backlog)
{
    (void) backlog;
    vfs_file_t* f = fd_get(fd);
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
    vfs_file_t* f = fd_get(fd);
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
    int nfd = fd_alloc_from(0);
    if (nfd < 0)
    {
        if (srv_rx->read_refs)
            srv_rx->read_refs--;
        pipe_maybe_free(srv_rx);
        pipe_drop_write(cli_rx);
        pipe_maybe_free(cli_rx);
        return -(int) EMFILE;
    }
    vfs_file_t* nf = file_alloc();
    if (!nf)
    {
        if (srv_rx->read_refs)
            srv_rx->read_refs--;
        pipe_maybe_free(srv_rx);
        pipe_drop_write(cli_rx);
        pipe_maybe_free(cli_rx);
        return -(int) ENOMEM;
    }
    nf->pipe = srv_rx;
    nf->wpipe = cli_rx;
    nf->pipe_end = PIPE_END_READ;
    nf->flags = O_RDWR | (flags & O_NONBLOCK);
    nf->cloexec = (flags & O_CLOEXEC) ? 1 : 0; /* accept4 SOCK_CLOEXEC */
    nf->peer_pid = peer_pid;
    nf->peer_uid = peer_uid;
    nf->peer_gid = peer_gid;
    g_fds[nfd] = nf;
    if (path_out && path_max > 0)
        path_out[0] = '\0';
    return nfd;
}

int fd_connect_unix(int fd, const char* path)
{
    vfs_file_t* f = fd_get(fd);
    if (!f || !f->node || f->node->type != VFS_TYPE_SOCK)
        return -(int) EBADF;

    vfs_node_t* sn;
    if (path[0] == '\0')
    {
        sn = NULL;
        for (int i = 0; i < MAX_ABSTRACT_SOCKS; i++)
        {
            if (g_abstract_socks[i].node && strncmp(g_abstract_socks[i].name, path + 1, 106) == 0)
            {
                sn = g_abstract_socks[i].node;
                break;
            }
        }
    }
    else
    {
        sn = vfs_lookup(path);
    }
    if (!sn || sn->type != VFS_TYPE_SOCK)
        return -(int) ECONNREFUSED;
    unix_sock_t* srv = (unix_sock_t*) sn->data;
    if (!srv || srv->state != SOCK_LISTENING)
        return -(int) ECONNREFUSED;
    pipe_t* cli_rx = pipe_alloc();
    pipe_t* srv_rx = pipe_alloc();
    if (!cli_rx || !srv_rx)
    {
        kfree(cli_rx);
        kfree(srv_rx);
        return -(int) ENOMEM;
    }
    cli_rx->read_refs = 1;
    cli_rx->write_refs = 1;
    srv_rx->read_refs = 1;
    srv_rx->write_refs = 1;
    unix_conn_t* conn = (unix_conn_t*) kcalloc(1, sizeof(unix_conn_t));
    if (!conn)
    {
        kfree(cli_rx);
        kfree(srv_rx);
        return -(int) ENOMEM;
    }
    conn->cli_rx = cli_rx;
    conn->srv_rx = srv_rx;
    conn->peer_pid = g_current_proc ? g_current_proc->pid : 0;
    conn->peer_uid = g_current_proc ? g_current_proc->uid : 0;
    conn->peer_gid = g_current_proc ? g_current_proc->gid : 0;
    if (!srv->backlog)
    {
        srv->backlog = conn;
    }
    else
    {
        unix_conn_t* tail = srv->backlog;
        while (tail->next)
            tail = tail->next;
        tail->next = conn;
    }
    sn->sock_backlog++;
    if (srv->accept_waiter && srv->accept_waiter->state == PROC_WAITING)
        srv->accept_waiter->state = PROC_READY;
    /* wake any process blocked in select()/poll() watching this socket */
    for (int _i = 0; _i < PROC_MAX; _i++)
        if (g_proctable[_i].state == PROC_WAITING)
            g_proctable[_i].state = PROC_READY;
    /* convert client fd from listening node to connected pipe fd */
    unix_sock_t* cs = (unix_sock_t*) f->node->data;
    kfree(cs);
    kfree(f->node);
    f->node = NULL;
    f->pipe = cli_rx;
    f->wpipe = srv_rx;
    f->pipe_end = PIPE_END_READ;
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
        const char* leaf;
        vfs_node_t* parent = parent_of(abspath[0] ? abspath : path, &leaf);
        (void) leaf;
        if (!parent || !may_create_in(parent))
            return -(int) EACCES;
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
        int acc = (flags & O_ACCMODE);
        uint32_t need = (acc != O_WRONLY ? 4u : 0u) | (acc != O_RDONLY ? 2u : 0u);
        if ((flags & O_TRUNC) && n->type == VFS_TYPE_REG)
            need |= 2u;
        if (!may_access(n, need))
            return -(int) EACCES;
    }

    int fd = fd_alloc_from(3);
    if (fd < 0)
        return -(int) EMFILE;

    vfs_file_t* f = file_alloc();
    if (!f)
        return -(int) ENOMEM;

    f->node = n;
    f->flags = flags;
    f->pos = 0;
    f->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    if ((flags & O_TRUNC) && n->type == VFS_TYPE_REG)
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
    if (!uptr_ok_w(buf, len)) /* kernel writes into buf: needs a writable page */
        return -(int64_t) EFAULT;
    if (!f->pipe && !f->wpipe && (f->flags & O_ACCMODE) == O_WRONLY)
        return -(int64_t) EBADF;

    if (f->wpipe)
    { /* socket */
        if ((f->flags & O_NONBLOCK) && f->pipe->count == 0 && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_read(f->pipe, buf, len);
    }

    /* Pipe */
    if (f->pipe)
    {
        if (f->pipe_end != PIPE_END_READ)
            return -(int64_t) EBADF;
        if ((f->flags & O_NONBLOCK) && f->pipe->count == 0 && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_read(f->pipe, buf, len);
    }

    vfs_node_t* n = f->node;
    if (n->type == VFS_TYPE_CHR)
    {
        if (!n->chr_read)
            return 0;
        if ((f->flags & O_NONBLOCK) && n->chr_pollin && !n->chr_pollin(n))
            return -(int64_t) EAGAIN;
        int64_t r = n->chr_read(n, (char*) buf, len, f->pos);
        if (r > 0)
            f->pos += (uint64_t) r;
        return r;
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

int64_t fd_peek(int fd, void* buf, uint64_t len, uint64_t skip)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (len == 0)
        return 0;
    if (!uptr_ok_w(buf, len)) /* kernel writes into buf: needs a writable page */
        return -(int64_t) EFAULT;

    if (f->wpipe)
    {
        if ((f->flags & O_NONBLOCK) && f->pipe->count <= skip && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_peek(f->pipe, buf, len, skip);
    }

    if (f->pipe)
    {
        if (f->pipe_end != PIPE_END_READ)
            return -(int64_t) EBADF;
        if ((f->flags & O_NONBLOCK) && f->pipe->count <= skip && f->pipe->write_refs > 0)
            return -(int64_t) EAGAIN;
        return pipe_peek(f->pipe, buf, len, skip);
    }

    return -(int64_t) EINVAL;
}

/* core write dispatch; buf already validated (or kernel-trusted) by caller */
static int64_t fd_write_dispatch(vfs_file_t* f, const void* buf, uint64_t len)
{
    if (!f->pipe && !f->wpipe && (f->flags & O_ACCMODE) == O_RDONLY)
        return -(int64_t) EBADF;

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

int64_t fd_write(int fd, const void* buf, uint64_t len)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (len == 0)
        return 0;
    if (!uptr_ok(buf, len))
        return -(int64_t) EFAULT;
    /* non-blocking socket: never block in pipe_write — return EAGAIN when the send
       buffer is full, else clamp to the free space so the write completes at once.
       lets XCB drain incoming events instead of deadlocking on a full buffer. */
    if (f->wpipe && (f->flags & O_NONBLOCK) && f->wpipe->read_refs > 0)
    {
        uint64_t space = PIPE_BUFSZ - f->wpipe->count;
        if (space == 0)
            return -(int64_t) EAGAIN;
        if (len > space)
            len = space;
    }
    return fd_write_dispatch(f, buf, len);
}

/* write from a kernel-trusted buffer (e.g. sendfile); skips user-pointer check */
int64_t fd_write_kbuf(int fd, const void* buf, uint64_t len)
{
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int64_t) EBADF;
    if (len == 0)
        return 0;
    return fd_write_dispatch(f, buf, len);
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
    if (!uptr_ok_w(st, sizeof(*st)))
        return -(int) EFAULT;
    if (f->wpipe)
    { /* connected socket */
        memset(st, 0, sizeof(*st));
        st->st_mode = S_IFSOCK | 0666;
        st->st_blksize = PIPE_BUFSZ;
        return 0;
    }
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
    if (!uptr_ok_w(st, sizeof(*st)))
        return -(int) EFAULT;
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
    if (!uptr_ok_w(st, sizeof(*st)))
        return -(int) EFAULT;
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
    if (!uptr_ok_w(st, sizeof(*st)))
        return -(int) EFAULT;
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n)
        return -(int) ENOENT;
    fill_stat(n, st);
    return 0;
}

int fd_getdents64(int fd, void* buf, uint64_t count)
{
    if (!buf || !count)
        return -(int) EINVAL;
    if (!uptr_ok_w(buf, count))
        return -(int) EFAULT;
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    if (f->pipe)
        return -(int) ENOTDIR;
    vfs_node_t* dir = f->node;
    if (dir->type != VFS_TYPE_DIR)
        return -(int) ENOTDIR;

    int proc_ret = 0;
    if (procfs_getdents64(dir, &f->pos, buf, count, &proc_ret))
        return proc_ret;

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
    if (!uptr_ok_w(buf, bufsz))
        return -(int) EFAULT;
    int proc_ret = 0;
    if (procfs_readlink(path, buf, bufsz, &proc_ret))
        return proc_ret;
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
#define FIONBIO 0x5421
#define FIOCLEX 0x5451

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

    if (f->node && f->node->type == VFS_TYPE_CHR && f->node->chr_ioctl)
        return (int) f->node->chr_ioctl(f->node, req, arg);

    switch (req)
    {
    case TIOCGWINSZ:
    {
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws)
            return -(int) EINVAL;
        if (!uptr_ok_w(ws, sizeof(*ws)))
            return -(int) EFAULT;
        *ws = g_pty_winsize;
        return 0;
    }
    case TIOCSWINSZ:
    {
        struct winsize* ws = (struct winsize*) (uintptr_t) arg;
        if (!ws || !uptr_ok(ws, sizeof(*ws)))
            return -(int) EFAULT;
        g_pty_winsize = *ws;
        return 0;
    }
    case TCGETS:
    {
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
    case 0x5404: /* TCSETSF */
    {
        struct termios* t = (struct termios*) (uintptr_t) arg;
        if (t)
        {
            if (!uptr_ok(t, sizeof(*t)))
                return -(int) EFAULT;
            tty_set_lflag(t->c_lflag);
        }
        return 0;
    }
    case 0x5405: /* TCGETA  */
    case 0x5406: /* TCSETA  */
    case 0x540B: /* TIOCSCTTY */
    case 0x5422: /* TIOCNOTTY */
    case 0x541B: /* FIONREAD */
    {
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
    case TIOCGPGRP:
    {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid)
        {
            if (!uptr_ok_w(pgid, sizeof(*pgid)))
                return -(int) EFAULT;
            *pgid = tty_get_fg_pgid();
        }
        return 0;
    }
    case TIOCSPGRP:
    {
        int* pgid = (int*) (uintptr_t) arg;
        if (pgid)
        {
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
    vfs_file_t* f = fd_get(fd);
    if (!f)
        return -(int) EBADF;
    switch (cmd)
    {
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
    case F_DUPFD_CLOEXEC:
    {
        int newfd = fd_alloc_from((int) arg);
        if (newfd < 0)
            return -(int) EMFILE;
        vfs_file_t* nf = file_alloc();
        if (!nf)
            return -(int) ENOMEM;
        *nf = *f;
        nf->magic = VFS_FILE_MAGIC;
        nf->cloexec = (cmd == F_DUPFD_CLOEXEC) ? 1 : 0; /* dup'd fd: cloexec per cmd */
        file_addref(nf);
        g_fds[newfd] = nf;
        return newfd;
    }
    case F_GETLK:
        return 0; /* advisory locks: always unlocked */
    case F_SETLK:
    case F_SETLKW:
        return 0; /* advisory locks: always succeed */
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
    vfs_file_t* nf = file_alloc();
    if (!nf)
        return -(int) ENOMEM;
    *nf = *f;
    nf->magic = VFS_FILE_MAGIC;
    nf->cloexec = 0; /* dup2 always clears FD_CLOEXEC on the new fd */
    file_addref(nf);
    g_fds[newfd] = nf;
    return newfd;
}

int fd_dup3(int oldfd, int newfd, int flags)
{
    if (oldfd == newfd)
        return -(int) EINVAL;
    int r = fd_dup2(oldfd, newfd);
    if (r >= 0)
    {
        vfs_file_t* nf = fd_get(r);
        if (nf)
            nf->cloexec = (flags & O_CLOEXEC) ? 1 : 0;
    }
    return r;
}

/* Reconstruct absolute path of node by walking parent pointers. */
char* vfs_node_abspath(vfs_node_t* n, char* buf, size_t sz)
{
    if (!n || !buf || sz == 0)
        return NULL;
    if (n->parent == n)
    { /* root */
        buf[0] = '/';
        buf[1] = '\0';
        return buf;
    }

    /* collect ancestors */
    vfs_node_t* stack[128];
    int depth = 0;
    vfs_node_t* cur = n;
    while (cur && cur->parent != cur && depth < 128)
    {
        stack[depth++] = cur;
        cur = cur->parent;
    }

    char* p = buf;
    char* end = buf + sz - 1;
    for (int i = depth - 1; i >= 0; i--)
    {
        if (p < end)
            *p++ = '/';
        size_t nl = strlen(stack[i]->name);
        if (p + nl > end)
            nl = (size_t) (end - p);
        memcpy(p, stack[i]->name, nl);
        p += nl;
    }
    if (p == buf)
        *p++ = '/';
    *p = '\0';
    return buf;
}

int vfs_link(const char* oldpath, const char* newpath)
{
    vfs_node_t* src = vfs_lookup(oldpath);
    if (!src)
        return -(int) ENOENT;
    if (src->type == VFS_TYPE_DIR)
        return -(int) EISDIR;
    const char* leaf;
    vfs_node_t* parent = parent_of(newpath, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR)
        return -(int) ENOENT;
    if (!leaf || !*leaf)
        return -(int) EINVAL;
    if (!may_create_in(parent))
        return -(int) EACCES;
    if (dir_find(parent, leaf))
        return -(int) EEXIST;
    /* hard link: create new node sharing same data buffer — shallow copy */
    vfs_node_t* ln = node_alloc(leaf, src->type, src->mode);
    if (!ln)
        return -(int) ENOMEM;
    ln->data = src->data; /* shared reference (no refcount — simple impl) */
    ln->size = src->size;
    ln->capacity = src->capacity;
    if (src->type == VFS_TYPE_REG && src->data && src->size > 0) {
        ln->data = (uint8_t*)kmalloc(src->size);
        if (!ln->data) { kfree(ln); return -(int)ENOMEM; }
        memcpy(ln->data, src->data, src->size);
        ln->capacity = src->size;
    } else if (src->type == VFS_TYPE_SYM && src->symlink) {
        size_t len = strlen(src->symlink) + 1;
        ln->symlink = (char*)kmalloc(len);
        if (!ln->symlink) { kfree(ln); return -(int)ENOMEM; }
        memcpy(ln->symlink, src->symlink, len);
        ln->size = len - 1;
        ln->capacity = 0;
    }
    dir_insert(parent, ln);
    return 0;
}

int vfs_chmod(const char* path, uint32_t mode)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    if (!may_change_mode(n))
        return -(int) EPERM;
    n->mode = (n->mode & ~07777U) | mode_without_priv_bits(mode);
    return 0;
}

int vfs_fchmod(int fd, uint32_t mode)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n)
        return -(int) EBADF;
    if (!may_change_mode(n))
        return -(int) EPERM;
    n->mode = (n->mode & ~07777U) | mode_without_priv_bits(mode);
    return 0;
}

int vfs_chown(const char* path, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    if (!may_change_owner(n))
        return -(int) EPERM;
    if (uid != (uint32_t) -1)
        n->uid = uid;
    if (gid != (uint32_t) -1)
        n->gid = gid;
    n->mode &= ~06000U;
    return 0;
}

int vfs_lchown(const char* path, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = vfs_lookup_nofollow(path);
    if (!n)
        return -(int) ENOENT;
    if (!may_change_owner(n))
        return -(int) EPERM;
    if (uid != (uint32_t) -1)
        n->uid = uid;
    if (gid != (uint32_t) -1)
        n->gid = gid;
    n->mode &= ~06000U;
    return 0;
}

int vfs_fchown(int fd, uint32_t uid, uint32_t gid)
{
    vfs_node_t* n = fd_get_node(fd);
    if (!n)
        return -(int) EBADF;
    if (!may_change_owner(n))
        return -(int) EPERM;
    if (uid != (uint32_t) -1)
        n->uid = uid;
    if (gid != (uint32_t) -1)
        n->gid = gid;
    n->mode &= ~06000U;
    return 0;
}

int vfs_truncate(const char* path, uint64_t len)
{
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    if (n->type != VFS_TYPE_REG)
        return -(int) EINVAL;
    if (!may_access(n, 2u))
        return -(int) EACCES;
    if (len < n->size)
        n->size = len;
    return 0;
}

int vfs_access(const char* path, int mode)
{
    if (mode & ~7)
        return -(int) EINVAL;
    vfs_node_t* n = vfs_lookup(path);
    if (!n)
        return -(int) ENOENT;
    if (mode == 0)
        return 0;
    if ((mode & 1) && !(n->mode & 0111U))
        return -(int) EACCES;
    return may_access(n, (uint32_t) mode) ? 0 : -(int) EACCES;
}

int vfs_mknod(const char* path, uint32_t mode, uint64_t dev)
{
    (void) dev;
    const char* leaf;
    vfs_node_t* parent = parent_of(path, &leaf);
    if (!parent || parent->type != VFS_TYPE_DIR)
        return -(int) ENOENT;
    if (!leaf || !*leaf)
        return -(int) EINVAL;
    if (!cred_is_root())
        return -(int) EPERM;
    if (!may_create_in(parent))
        return -(int) EACCES;
    if (dir_find(parent, leaf))
        return -(int) EEXIST;
    uint32_t ftype = mode & S_IFMT;
    if (!ftype)
        ftype = S_IFREG;
    uint8_t type = (ftype == S_IFDIR) ? VFS_TYPE_DIR : VFS_TYPE_REG;
    vfs_node_t* n = node_alloc(leaf, type, apply_umask(mode & 07777U) | ftype);
    if (!n)
        return -(int) ENOMEM;
    dir_insert(parent, n);
    return 0;
}

/* Resolve *at dirfd+path into an absolute path stored in out[sz]. */
int at_resolve(int dirfd, const char* path, char* out, size_t sz)
{
    if (!path)
        return -(int) EFAULT;
    if (path[0] == '/' || dirfd == AT_FDCWD)
    {
        vfs_abs_path(out, sz, path);
        return 0;
    }
    vfs_file_t* df = fd_get(dirfd);
    if (!df || !df->node || df->node->type != VFS_TYPE_DIR)
        return -(int) EBADF;
    char dirpath[512];
    if (!vfs_node_abspath(df->node, dirpath, sizeof(dirpath)))
        return -(int) EINVAL;
    size_t dl = strlen(dirpath);
    if (dl + 1 + strlen(path) >= sz)
        return -(int) ENAMETOOLONG;
    memcpy(out, dirpath, dl);
    if (out[dl - 1] != '/')
        out[dl++] = '/';
    strcpy(out + dl, path);
    return 0;
}
