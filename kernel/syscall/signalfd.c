#include "signalfd.h"
#include "internal.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/spinlock.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "proc/signal.h"

static signalfd_inst_t g_signalfd[SFD_MAX_INSTANCES];
static spinlock_t g_sfd_lock = SPINLOCK_INIT;

static int64_t signalfd_chr_read(vfs_node_t *n, char *buf, uint64_t len, uint64_t off) {
    (void)off;
    if (!n || !n->fs_private) return -(int64_t)EINVAL;
    int idx = (int)(uintptr_t)n->fs_private - 1;
    if (idx < 0 || idx >= SFD_MAX_INSTANCES || !g_signalfd[idx].used) return -(int64_t)EINVAL;
    if (!buf || len < sizeof(struct signalfd_siginfo)) return -(int64_t)EINVAL;
    if (!uptr_ok_w(buf, len)) return -(int64_t)EFAULT;
    spin_lock(&g_sfd_lock);
    signalfd_inst_t *inst = &g_signalfd[idx];
    if (inst->pending_count == 0) { spin_unlock(&g_sfd_lock); return 0; }
    struct signalfd_siginfo *si = &inst->pending[inst->pending_tail];
    uint64_t copy_bytes = sizeof(struct signalfd_siginfo);
    if (copy_bytes > len) copy_bytes = len;
    inst->pending_tail = (inst->pending_tail + 1) % 16;
    inst->pending_count--;
    spin_unlock(&g_sfd_lock);
    memcpy(buf, si, copy_bytes);
    return (int64_t)copy_bytes;
}

void signalfd_deliver(int fd, int sig) {
    if (fd < 0 || sig < 1 || sig > 64) return;
    spin_lock(&g_sfd_lock);
    for (int i = 0; i < SFD_MAX_INSTANCES; i++) {
        if (!g_signalfd[i].used || g_signalfd[i].fd != fd) continue;
        if (!(g_signalfd[i].sigmask & (1ULL << (sig - 1)))) continue;
        if (g_signalfd[i].pending_count >= 16) { spin_unlock(&g_sfd_lock); return; }
        struct signalfd_siginfo *si = &g_signalfd[i].pending[g_signalfd[i].pending_head];
        memset(si, 0, sizeof(*si));
        si->ssi_signo = (uint32_t)sig;
        si->ssi_pid = cur() ? cur()->pid : 0;
        si->ssi_uid = cur() ? cur()->uid : 0;
        g_signalfd[i].pending_head = (g_signalfd[i].pending_head + 1) % 16;
        g_signalfd[i].pending_count++;
        break;
    }
    spin_unlock(&g_sfd_lock);
}

int sys_signalfd(int fd, const uint64_t *mask, uint32_t flags) {
    proc_t *p = cur();
    if (!p) return -(int)EFAULT;
    if (fd >= 0) {
        vfs_file_t *f = fd_get_file(fd);
        if (f && f->node && f->node->fs_private) {
            spin_lock(&g_sfd_lock);
            for (int i = 0; i < SFD_MAX_INSTANCES; i++) {
                if (g_signalfd[i].used && g_signalfd[i].fd == fd) {
                    if (mask && uptr_ok(mask, 8)) g_signalfd[i].sigmask = *mask;
                    spin_unlock(&g_sfd_lock);
                    return fd;
                }
            }
            spin_unlock(&g_sfd_lock);
        }
    }
    uint64_t sigmask = 0;
    if (mask) { if (!uptr_ok(mask, 8)) return -(int)EFAULT; sigmask = *mask; }
    int idx = -1;
    spin_lock(&g_sfd_lock);
    for (int i = 0; i < SFD_MAX_INSTANCES; i++) {
        if (!g_signalfd[i].used) { idx = i; g_signalfd[i].used = 1; break; }
    }
    spin_unlock(&g_sfd_lock);
    if (idx < 0) return -(int)ENOMEM;
    vfs_node_t *node = (vfs_node_t *)kmalloc(sizeof(vfs_node_t));
    if (!node) { g_signalfd[idx].used = 0; return -(int)ENOMEM; }
    memset(node, 0, sizeof(*node));
    node->type = VFS_TYPE_CHR;
    node->mode = S_IFCHR | 0600;
    node->ino = 91000 + (uint32_t)idx;
    node->uid = p->uid;
    node->gid = p->gid;
    node->chr_read = signalfd_chr_read;
    node->fs_private = (void *)(uintptr_t)(idx + 1);
    int open_flags = O_RDONLY;
    if (flags & 0x800) open_flags |= O_CLOEXEC;
    if (flags & 0x80000) open_flags |= O_NONBLOCK;
    int newfd = fd_open_node(node, open_flags);
    if (newfd < 0) { g_signalfd[idx].used = 0; kfree(node); return newfd; }
    spin_lock(&g_sfd_lock);
    g_signalfd[idx].sigmask = sigmask;
    g_signalfd[idx].fd = newfd;
    g_signalfd[idx].pending_count = 0;
    g_signalfd[idx].pending_head = 0;
    g_signalfd[idx].pending_tail = 0;
    spin_unlock(&g_sfd_lock);
    return newfd;
}
