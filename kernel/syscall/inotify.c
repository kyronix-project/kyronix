#include "inotify.h"
#include "internal.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"

static inotify_instance_t g_inotify[INOTIFY_MAX_INSTANCES];
static spinlock_t g_inotify_lock = SPINLOCK_INIT;

static int64_t inotify_chr_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void)off;
    if (!n || !n->fs_private) return -(int64_t)EINVAL;
    int idx = (int)(uintptr_t)n->fs_private - 1;
    if (idx < 0 || idx >= INOTIFY_MAX_INSTANCES || !g_inotify[idx].used) return -(int64_t)EINVAL;
    inotify_instance_t *inst = &g_inotify[idx];
    spin_lock(&g_inotify_lock);
    uint32_t avail = inst->tail;
    if (avail == 0) { spin_unlock(&g_inotify_lock); return 0; }
    if (avail > len) avail = (uint32_t)len;
    memcpy(buf, inst->ring, avail);
    if (avail < inst->tail) memmove(inst->ring, inst->ring + avail, inst->tail - avail);
    inst->tail -= avail;
    spin_unlock(&g_inotify_lock);
    return (int64_t)avail;
}

void inotify_emit(int fd, int32_t wd, uint32_t mask, uint32_t cookie, const char *name) {
    if (fd < 0) return;
    spin_lock(&g_inotify_lock);
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!g_inotify[i].used) continue;
        bool found = false;
        for (int j = 0; j < INOTIFY_MAX_WATCHES; j++) {
            if (g_inotify[i].watches[j].used && g_inotify[i].watches[j].wd == wd) {
                found = true;
                break;
            }
        }
        if (!found) continue;
        inotify_instance_t *inst = &g_inotify[i];
        uint32_t name_len = name ? (uint32_t)strlen(name) + 1 : 0;
        uint32_t event_size = (uint32_t)(sizeof(struct inotify_event) + name_len);
        uint32_t aligned = (event_size + 7) & ~7U;
        if (inst->tail + aligned > INOTIFY_BUFSZ) {
            inst->overflow++;
            spin_unlock(&g_inotify_lock);
            return;
        }
        struct inotify_event *ev = (struct inotify_event *)(inst->ring + inst->tail);
        ev->wd = wd;
        ev->mask = mask;
        ev->cookie = cookie;
        ev->len = name_len;
        if (name && name_len > 0) memcpy(ev->name, name, name_len);
        inst->tail += aligned;
    }
    spin_unlock(&g_inotify_lock);
}

static int inotify_alloc_instance(void) {
    for (int i = 0; i < INOTIFY_MAX_INSTANCES; i++) {
        if (!g_inotify[i].used) {
            memset(&g_inotify[i], 0, sizeof(inotify_instance_t));
            g_inotify[i].used = 1;
            return i;
        }
    }
    return -1;
}

int sys_inotify_init(void) {
    spin_lock(&g_inotify_lock);
    int idx = inotify_alloc_instance();
    spin_unlock(&g_inotify_lock);
    if (idx < 0) return -(int)ENOMEM;
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) { spin_lock(&g_inotify_lock); g_inotify[idx].used = 0; spin_unlock(&g_inotify_lock); return -(int)ENOMEM; }
    memset(node, 0, sizeof(*node));
    node->type = VFS_TYPE_CHR;
    node->mode = S_IFCHR | 0600;
    node->ino = 90000 + (uint32_t)idx;
    node->uid = 0;
    node->gid = 0;
    node->chr_read = inotify_chr_read;
    node->fs_private = (void *)(uintptr_t)(idx + 1);
    g_inotify[idx].flags = 0;
    return fd_open_node(node, O_RDONLY);
}

int sys_inotify_init1(int flags) {
    spin_lock(&g_inotify_lock);
    int idx = inotify_alloc_instance();
    spin_unlock(&g_inotify_lock);
    if (idx < 0) return -(int)ENOMEM;
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) { spin_lock(&g_inotify_lock); g_inotify[idx].used = 0; spin_unlock(&g_inotify_lock); return -(int)ENOMEM; }
    memset(node, 0, sizeof(*node));
    node->type = VFS_TYPE_CHR;
    node->mode = S_IFCHR | 0600;
    node->ino = 90000 + (uint32_t)idx;
    node->uid = 0;
    node->gid = 0;
    node->chr_read = inotify_chr_read;
    node->fs_private = (void *)(uintptr_t)(idx + 1);
    int open_flags = O_RDONLY;
    if (flags & 0x80000) open_flags |= O_NONBLOCK;
    if (flags & 0x800) open_flags |= O_CLOEXEC;
    g_inotify[idx].flags = flags;
    return fd_open_node(node, open_flags);
}

int sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask) {
    if (!pathname) return -(int)EFAULT;
    vfs_file_t *f = fd_get_file(fd);
    if (!f || !f->node) return -(int)EBADF;
    void *priv = f->node->fs_private;
    if (!priv) return -(int)EINVAL;
    int idx = (int)(uintptr_t)priv - 1;
    if (idx < 0 || idx >= INOTIFY_MAX_INSTANCES || !g_inotify[idx].used) return -(int)EINVAL;
    char kpath[512];
    if (!path_abs(kpath, pathname)) return -(int)EFAULT;
    spin_lock(&g_inotify_lock);
    inotify_instance_t *inst = &g_inotify[idx];
    int wd = -1;
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (!inst->watches[i].used) {
            inst->watches[i].used = 1;
            inst->watches[i].wd = i + 1;
            inst->watches[i].mask = mask;
            strncpy(inst->watches[i].path, kpath, sizeof(inst->watches[i].path) - 1);
            inst->watches[i].path[sizeof(inst->watches[i].path) - 1] = '\0';
            wd = i + 1;
            break;
        }
    }
    spin_unlock(&g_inotify_lock);
    return wd < 0 ? -(int)ENOSPC : wd;
}

int sys_inotify_rm_watch(int fd, int wd) {
    vfs_file_t *f = fd_get_file(fd);
    if (!f || !f->node) return -(int)EBADF;
    void *priv = f->node->fs_private;
    if (!priv) return -(int)EINVAL;
    int idx = (int)(uintptr_t)priv - 1;
    if (idx < 0 || idx >= INOTIFY_MAX_INSTANCES || !g_inotify[idx].used) return -(int)EINVAL;
    spin_lock(&g_inotify_lock);
    inotify_instance_t *inst = &g_inotify[idx];
    for (int i = 0; i < INOTIFY_MAX_WATCHES; i++) {
        if (inst->watches[i].used && inst->watches[i].wd == wd) {
            inst->watches[i].used = 0;
            inotify_emit(fd, wd, IN_IGNORED, 0, NULL);
            spin_unlock(&g_inotify_lock);
            return 0;
        }
    }
    spin_unlock(&g_inotify_lock);
    return -(int)EINVAL;
}
