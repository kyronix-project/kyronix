#include "mount.h"
#include "drivers/block.h"
#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"

#define EINVAL 22
#define EFAULT 14
#define EBUSY 16
#define ENODEV 19
#define ENOENT 2
#define EPERM 1

static mount_entry_t g_mounts[MOUNT_MAX];
static int g_mount_count = 0;

mount_entry_t *mount_table(void) { return g_mounts; }
int mount_count(void) { return g_mount_count; }

void mount_add(const char *source, const char *target, const char *fstype, uint32_t flags) {
    if (g_mount_count >= MOUNT_MAX) return;
    mount_entry_t *m = &g_mounts[g_mount_count++];
    memset(m, 0, sizeof(*m));
    strncpy(m->source, source, sizeof(m->source) - 1);
    strncpy(m->target, target, sizeof(m->target) - 1);
    strncpy(m->fstype, fstype, sizeof(m->fstype) - 1);
    m->flags = flags;
    m->used = true;
}

void mount_remove(const char *target) {
    for (int i = 0; i < g_mount_count; i++) {
        if (g_mounts[i].used && strcmp(g_mounts[i].target, target) == 0) {
            g_mounts[i].used = false;
            if (i == g_mount_count - 1) g_mount_count--;
            return;
        }
    }
}

mount_entry_t *mount_find(const char *target) {
    for (int i = g_mount_count - 1; i >= 0; i--) {
        if (g_mounts[i].used && strcmp(g_mounts[i].target, target) == 0) return &g_mounts[i];
    }
    return NULL;
}

static bool copy_user_string(char *out, const char *in, int max) {
    if (!in) return false;
    for (int i = 0; i < max - 1; i++) {
        const char *a = in + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return false;
        }
        char c = *a;
        out[i] = c;
        if (!c) return true;
    }
    out[max - 1] = '\0';
    return true;
}

int64_t sys_mount(const char *source, const char *target, const char *fstype, uint64_t flags,
                  const void *data) {
    (void) data;

    if (!host_priv()) return -(int64_t) EPERM;

    char ksrc[64], ktarget[256], kfstype[16];
    if (!copy_user_string(ksrc, source, sizeof(ksrc))) return -(int64_t) EFAULT;
    if (!copy_user_string(ktarget, target, sizeof(ktarget))) return -(int64_t) EFAULT;
    if (!copy_user_string(kfstype, fstype, sizeof(kfstype))) return -(int64_t) EFAULT;

    if (!ksrc[0] || !ktarget[0] || !kfstype[0]) return -(int64_t) EINVAL;

    const char *device_name = ksrc;
    if (strncmp(device_name, "/dev/", 5) == 0) device_name += 5;
    struct block_device *bd = block_by_name(device_name);
    if (!bd) {
        log_warn("mount: device '%s' not found", ksrc);
        return -(int64_t) ENODEV;
    }

    struct filesystem *fs = vfs_find_fs(kfstype);
    if (!fs) {
        log_warn("mount: filesystem '%s' not supported", kfstype);
        return -(int64_t) EINVAL;
    }

    if (!fs->mount) return -(int64_t) EINVAL;

    vfs_mkdir_p(ktarget, 0755);

    bool ok = fs->mount(bd, ktarget);
    if (!ok) {
        log_warn("mount: failed to mount %s on %s", ksrc, ktarget);
        return -(int64_t) EINVAL;
    }

    mount_add(ksrc, ktarget, kfstype, (uint32_t) flags);
    log_info("mount: %s on %s type %s", ksrc, ktarget, kfstype);
    return 0;
}

int64_t sys_umount2(const char *target, int flags) {
    (void) flags;

    if (!host_priv()) return -(int64_t) EPERM;

    char ktarget[256];
    if (!copy_user_string(ktarget, target, sizeof(ktarget))) return -(int64_t) EFAULT;

    if (!ktarget[0]) return -(int64_t) EINVAL;

    mount_entry_t *m = mount_find(ktarget);
    if (!m) return -(int64_t) EINVAL;

    struct filesystem *fs = vfs_find_fs(m->fstype);
    if (fs && fs->unmount && !fs->unmount(ktarget))
        return -(int64_t) EBUSY;

    mount_remove(ktarget);
    log_info("umount: %s", ktarget);
    return 0;
}
