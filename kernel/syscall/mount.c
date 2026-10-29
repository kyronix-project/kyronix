#include "mount.h"

#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "proc/jail.h"
#include "proc/ns.h"

#define ENODEV 19
#define EROFS 30

static bool copy_user_string(char *out, int max, const char *in) {
    if (!out || max <= 0) return false;
    if (!in) {
        out[0] = '\0';
        return true;
    }
    for (int i = 0; i < max - 1; i++) {
        const char *a = in + i;
        if (i == 0 || ((uint64_t) (uintptr_t) a & 0xFFF) == 0) {
            if (!uptr_ok(a, 1)) return false;
        }
        char c = *a;
        out[i] = c;
        if (!c) return true;
    }
    const char *last = in + max - 1;
    if (!uptr_ok(last, 1) || *last) return false;
    out[max - 1] = '\0';
    return true;
}

static bool make_abs(char *out, size_t sz, const char *in) {
    if (!in || !in[0]) {
        if (sz) out[0] = '\0';
        return true;
    }
    const char *root = jail_root_current();
    char tmp[512];
    if (in[0] == '/') {
        if (root[0])
            snprintf(tmp, sizeof(tmp), "%s%s", root, in);
        else
            strncpy(tmp, in, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    } else {
        proc_t *p = cur();
        const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";
        size_t cl = strlen(cwd);
        if (cl >= sizeof(tmp) - 1) return false;
        memcpy(tmp, cwd, cl);
        if (tmp[cl - 1] != '/') tmp[cl++] = '/';
        strncpy(tmp + cl, in, sizeof(tmp) - cl - 1);
        tmp[sizeof(tmp) - 1] = '\0';
    }
    if (root[0]) jail_canon_clamp(tmp, sizeof(tmp), root);
    char canon[512];
    path_canon(tmp, canon, sizeof(canon));
    strncpy(out, canon, sz - 1);
    out[sz - 1] = '\0';
    return true;
}

static bool mount_priv(void) {
    return host_priv() || ns_user_is_root(cur());
}

static bool path_under(const char *path, const char *prefix) {
    size_t len = strlen(prefix);
    return strncmp(path, prefix, len) == 0 && (path[len] == '\0' || path[len] == '/');
}

bool ns_mnt_resolve(mnt_namespace_t *ns, const char *path, struct vfs_node **node,
                    const char **suffix, uint32_t *flags) {
    if (!ns || !path || path[0] != '/') return false;
    if (!ns->root) ns->root = vfs_root_node();
    struct vfs_node *best = ns->root;
    const char *best_suffix = path;
    uint32_t best_flags = ns->root_flags;
    size_t best_len = 1;

    for (uint32_t i = 0; i < NS_MNT_MAX; i++) {
        mnt_mount_t *m = &ns->mounts[i];
        if (!m->used || !m->root) continue;
        size_t len = strlen(m->target);
        if (len < best_len) continue;
        if (!path_under(path, m->target)) continue;
        best = m->root;
        best_suffix = path + len;
        best_flags = m->flags;
        best_len = len;
    }
    if (node) *node = best;
    if (suffix) *suffix = best_suffix;
    if (flags) *flags = best_flags;
    return true;
}

uint32_t ns_mnt_path_flags(const char *path) {
    if (!path || path[0] != '/') return 0;
    char canon[512];
    path_canon(path, canon, sizeof(canon));
    uint32_t flags = 0;
    ns_mnt_resolve(ns_mnt_current(), canon, NULL, NULL, &flags);
    return flags;
}

bool ns_mnt_path_readonly(const char *path) {
    return (ns_mnt_path_flags(path) & MS_RDONLY) != 0;
}

static void mount_opts(char *out, size_t sz, uint32_t flags) {
    snprintf(out, sz, "%s%s%s%s", (flags & MS_RDONLY) ? "ro" : "rw",
             (flags & MS_NOSUID) ? ",nosuid" : "", (flags & MS_NODEV) ? ",nodev" : "",
             (flags & MS_NOEXEC) ? ",noexec" : "");
}

int ns_mnt_list(char *buf, uint64_t len, bool info) {
    if (!buf || len == 0) return 0;
    mnt_namespace_t *ns = ns_mnt_current();
    uint64_t pos = 0;
    char opts[64];
    mount_opts(opts, sizeof(opts), ns->root_flags);
    if (info) {
        pos += (uint64_t) snprintf(buf + pos, len - pos, "1 0 0:1 / / %s - rootfs rootfs %s\n",
                                   opts, opts);
    } else {
        pos += (uint64_t) snprintf(buf + pos, len - pos, "rootfs / rootfs %s 0 0\n", opts);
    }
    uint32_t id = 2;
    for (uint32_t i = 0; i < NS_MNT_MAX && pos < len; i++) {
        mnt_mount_t *m = &ns->mounts[i];
        if (!m->used) continue;
        mount_opts(opts, sizeof(opts), m->flags);
        const char *source = m->source[0] ? m->source : m->fstype;
        int n;
        if (info)
            n = snprintf(buf + pos, len - pos, "%u 1 0:2 / %s %s - %s %s %s\n", id++, m->target,
                         opts, m->fstype, source, opts);
        else
            n = snprintf(buf + pos, len - pos, "%s %s %s %s 0 0\n", source, m->target, m->fstype,
                         opts);
        if (n > 0) pos += (uint64_t) n;
        if (pos >= len) break;
    }
    return (int) (pos < len ? pos : len - 1);
}

static mnt_mount_t *mount_slot(mnt_namespace_t *ns) {
    for (uint32_t i = 0; i < NS_MNT_MAX; i++)
        if (!ns->mounts[i].used) return &ns->mounts[i];
    return NULL;
}

static int mount_attach(mnt_namespace_t *ns, const char *target, const char *source,
                        const char *fstype, struct vfs_node *root, uint32_t flags) {
    if (strcmp(target, "/") == 0) {
        if (ns->root) vfs_node_unref_internal(ns->root);
        ns->root = root;
        if (root) vfs_node_ref_internal(root);
        ns->root_flags = flags;
        return 0;
    }

    mnt_mount_t *m = ns_mnt_find(ns, target);
    if (!m) m = mount_slot(ns);
    if (!m) return -ENOSPC;
    if (m->used && m->root) vfs_node_unref_internal(m->root);
    memset(m, 0, sizeof(*m));
    strncpy(m->target, target, sizeof(m->target) - 1);
    strncpy(m->source, source, sizeof(m->source) - 1);
    strncpy(m->fstype, fstype, sizeof(m->fstype) - 1);
    m->root = root;
    if (root) vfs_node_ref_internal(root);
    m->flags = flags;
    m->used = 1;
    return 0;
}

static int mnt_rebase_root(mnt_namespace_t *ns, const char *newroot, struct vfs_node *node) {
    if (!ns || !node || !newroot) return -EINVAL;
    size_t plen = strlen(newroot);
    uint32_t new_flags = ns->root_flags;
    bool have_root_mount = false;

    mnt_mount_t *rootm = ns_mnt_find(ns, newroot);
    if (rootm) {
        new_flags = rootm->flags;
        have_root_mount = true;
        if (rootm->root) vfs_node_unref_internal(rootm->root);
        memset(rootm, 0, sizeof(*rootm));
    }

    for (uint32_t i = 0; i < NS_MNT_MAX; i++) {
        mnt_mount_t *m = &ns->mounts[i];
        if (!m->used) continue;
        if (have_root_mount && m == rootm) continue;
        if (strcmp(newroot, "/") != 0 && path_under(m->target, newroot)) {
            const char *rest = m->target + plen;
            if (!*rest) {
                if (m->root) vfs_node_unref_internal(m->root);
                memset(m, 0, sizeof(*m));
            } else {
                memmove(m->target, rest, strlen(rest) + 1);
            }
        } else if (strcmp(newroot, "/") != 0) {
            if (m->root) vfs_node_unref_internal(m->root);
            memset(m, 0, sizeof(*m));
        }
    }

    vfs_node_t *old_root = ns->root;
    ns->root = node;
    vfs_node_ref_internal(node);
    if (old_root) vfs_node_unref_internal(old_root);
    ns->root_flags = new_flags;
    return 0;
}

static uint32_t parse_mode(const char *data, uint32_t def) {
    if (!data) return def;
    const char *p = strstr(data, "mode=");
    if (!p) return def;
    p += 5;
    uint32_t mode = 0;
    int n = 0;
    while (*p >= '0' && *p <= '7' && n < 6) {
        mode = mode * 8 + (uint32_t) (*p - '0');
        p++;
        n++;
    }
    return n ? mode : def;
}

static vfs_node_t *bind_source(const char *path) {
    const char *num = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        num = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        num = path + 8;
    if (num && *num >= '0' && *num <= '9') {
        int fd = 0;
        while (*num >= '0' && *num <= '9') fd = fd * 10 + (*num++ - '0');
        if (*num == '\0') {
            vfs_node_t *n = fd_get_node(fd);
            if (n) vfs_node_ref_internal(n);
            return n;
        }
    }
    return vfs_lookup(path);
}

static bool fd_path_target(const char *path, char *out, size_t sz) {
    const char *num = NULL;
    if (strncmp(path, "/proc/self/fd/", 14) == 0)
        num = path + 14;
    else if (strncmp(path, "/dev/fd/", 8) == 0)
        num = path + 8;
    if (!num || !*num) return false;
    int fd = 0;
    while (*num >= '0' && *num <= '9') fd = fd * 10 + (*num++ - '0');
    if (*num != '\0') return false;
    vfs_node_t *n = fd_get_node(fd);
    if (!n) return false;
    bool ok = vfs_node_abspath(n, out, sz) != NULL;
    return ok;
}

static int ensure_target(const char *target, bool dir) {
    vfs_node_t *n = vfs_lookup(target);
    if (n) {
        bool is_dir = n->type == VFS_TYPE_DIR;
        vfs_node_unref_internal(n);
        return is_dir == dir ? 0 : (dir ? -ENOTDIR : -EISDIR);
    }
    if (!dir) {
        if (!vfs_create_file(target, 0600, NULL, 0)) return -ENOENT;
        return 0;
    }
    return vfs_mkdir(target, 0755);
}

int64_t sys_mount(const char *source, const char *target, const char *fstype, uint64_t flags,
                  const void *data) {
    if (!mount_priv()) return -(int64_t) EPERM;

    char usrc[512], utgt[512], ufstype[16], udata[128];
    if (!copy_user_string(usrc, sizeof(usrc), source)) return -(int64_t) EFAULT;
    if (!copy_user_string(utgt, sizeof(utgt), target)) return -(int64_t) EFAULT;
    if (!copy_user_string(ufstype, sizeof(ufstype), fstype)) return -(int64_t) EFAULT;
    if (!copy_user_string(udata, sizeof(udata), (const char *) data)) return -(int64_t) EFAULT;
    if (!utgt[0]) return -(int64_t) EINVAL;

    char ksrc[512], ktarget[512];
    if (!make_abs(ksrc, sizeof(ksrc), usrc)) return -(int64_t) EFAULT;
    if (!make_abs(ktarget, sizeof(ktarget), utgt)) return -(int64_t) EFAULT;
    char fdtgt[512];
    if (fd_path_target(ktarget, fdtgt, sizeof(fdtgt)))
        strncpy(ktarget, fdtgt, sizeof(ktarget) - 1);
    mnt_namespace_t *ns = ns_mnt_current();

    if (flags & MS_REMOUNT) {
        mnt_mount_t *m = ns_mnt_find(ns, ktarget);
        if (m)
            m->flags = (uint32_t) flags;
        else if (strcmp(ktarget, "/") == 0)
            ns->root_flags = (uint32_t) flags;
        else
            return -(int64_t) EINVAL;
        return 0;
    }

    if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)) return 0;

    if (flags & MS_MOVE) {
        mnt_mount_t *m = ns_mnt_find(ns, ksrc);
        if (!m) return -(int64_t) EINVAL;
        if (ensure_target(ktarget, m->root && m->root->type == VFS_TYPE_DIR) < 0)
            return -(int64_t) EINVAL;
        mnt_mount_t *dst = ns_mnt_find(ns, ktarget);
        if (!dst) dst = mount_slot(ns);
        if (!dst) return -(int64_t) ENOSPC;
        if (dst->used && dst->root) vfs_node_unref_internal(dst->root);
        *dst = *m;
        memset(m, 0, sizeof(*m));
        strncpy(dst->target, ktarget, sizeof(dst->target) - 1);
        return 0;
    }

    if (flags & MS_BIND) {
        if (!ksrc[0]) return -(int64_t) EINVAL;
        vfs_node_t *src = bind_source(ksrc);
        if (!src) return -(int64_t) ENOENT;
        int trc = ensure_target(ktarget, src->type == VFS_TYPE_DIR);
        if (trc < 0) {
            vfs_node_unref_internal(src);
            return trc;
        }
        int rc = mount_attach(ns, ktarget, ksrc, "none", src, (uint32_t) flags);
        vfs_node_unref_internal(src);
        if (rc < 0) return rc;
        return 0;
    }

    if (!ufstype[0]) return -(int64_t) EINVAL;

    vfs_node_t *root = NULL;
    const char *shown = ufstype;
    if (strcmp(ufstype, "proc") == 0) {
        root = vfs_proc_root();
        if (!root) return -(int64_t) ENODEV;
    } else if (strcmp(ufstype, "sysfs") == 0) {
        root = vfs_sys_root();
        if (!root) return -(int64_t) ENODEV;
    } else if (strcmp(ufstype, "devtmpfs") == 0) {
        root = vfs_dev_root();
        if (!root) return -(int64_t) ENODEV;
    } else if (strcmp(ufstype, "tmpfs") == 0 || strcmp(ufstype, "ramfs") == 0 ||
               strcmp(ufstype, "devpts") == 0 || strcmp(ufstype, "mqueue") == 0 ||
               strcmp(ufstype, "cgroup") == 0 || strcmp(ufstype, "cgroup2") == 0 ||
               strcmp(ufstype, "securityfs") == 0 || strcmp(ufstype, "debugfs") == 0 ||
               strcmp(ufstype, "tracefs") == 0 || strcmp(ufstype, "configfs") == 0 ||
               strcmp(ufstype, "bpf") == 0 || strcmp(ufstype, "hugetlbfs") == 0) {
        uint32_t def = strcmp(ufstype, "tmpfs") == 0 ? 01777 : 0755;
        root = vfs_create_mount_root(parse_mode(udata, def));
        if (!root) return -(int64_t) ENOMEM;
    } else {
        return -(int64_t) ENODEV;
    }

    int trc = ensure_target(ktarget, true);
    if (trc < 0) {
        vfs_node_unref_internal(root);
        return trc;
    }
    int rc = mount_attach(ns, ktarget, shown, ufstype, root, (uint32_t) flags);
    vfs_node_unref_internal(root);
    if (rc < 0) return rc;
    log_info("mount: %s on %s type %s", shown, ktarget, ufstype);
    return 0;
}

int64_t sys_umount2(const char *target, int flags) {
    if (!mount_priv()) return -(int64_t) EPERM;
    char utgt[512], ktarget[512];
    if (!copy_user_string(utgt, sizeof(utgt), target)) return -(int64_t) EFAULT;
    if (!utgt[0]) return -(int64_t) EINVAL;
    if (!make_abs(ktarget, sizeof(ktarget), utgt)) return -(int64_t) EFAULT;

    mnt_namespace_t *ns = ns_mnt_current();
    if (strcmp(ktarget, "/") == 0) return (flags & MNT_DETACH) ? 0 : -(int64_t) EBUSY;

    mnt_mount_t *m = ns_mnt_find(ns, ktarget);
    if (!m) return (flags & MNT_DETACH) ? 0 : -(int64_t) EINVAL;
    if (m->root) vfs_node_unref_internal(m->root);
    memset(m, 0, sizeof(*m));
    log_info("umount: %s", ktarget);
    return 0;
}

static int64_t change_root(const char *path, const char *put_old, bool pivot) {
    if (!mount_priv()) return -(int64_t) EPERM;
    char upath[512], kpath[512];
    if (!copy_user_string(upath, sizeof(upath), path)) return -(int64_t) EFAULT;
    if (!upath[0]) return -(int64_t) EINVAL;
    if (!make_abs(kpath, sizeof(kpath), upath)) return -(int64_t) EFAULT;

    vfs_node_t *node = vfs_lookup(kpath);
    if (!node) return -(int64_t) ENOENT;
    if (node->type != VFS_TYPE_DIR) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOTDIR;
    }

    mnt_namespace_t *ns = ns_mnt_current();
    mnt_namespace_t *target = pivot ? ns : ns_mnt_clone(ns);
    if (!target) {
        vfs_node_unref_internal(node);
        return -(int64_t) ENOMEM;
    }

    if (pivot && put_old && put_old[0]) {
        char uput[512], kput[512];
        if (!copy_user_string(uput, sizeof(uput), put_old) ||
            !make_abs(kput, sizeof(kput), uput)) {
            vfs_node_unref_internal(node);
            return -(int64_t) EFAULT;
        }
        if (strcmp(kput, kpath) != 0) {
            int arc = mount_attach(target, kput, "/", "none", target->root, target->root_flags);
            if (arc < 0) {
                vfs_node_unref_internal(node);
                return arc;
            }
        }
    }

    int rc = mnt_rebase_root(target, kpath, node);
    vfs_node_unref_internal(node);
    if (rc < 0) {
        if (!pivot) ns_mnt_put(target);
        return rc;
    }
    if (!pivot) {
        ns_mnt_set(cur(), target);
        ns_mnt_put(target);
    }

    proc_t *p = cur();
    if (p) {
        p->cwd[0] = '/';
        p->cwd[1] = '\0';
    }
    return 0;
}

int64_t sys_chroot(const char *path) { return change_root(path, NULL, false); }

int64_t sys_pivot_root(const char *new_root, const char *put_old) {
    return change_root(new_root, put_old, true);
}
