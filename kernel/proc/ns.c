#include "ns.h"

#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "syscall/internal.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"

user_namespace_t init_user_ns = { .common = { .type = NS_TYPE_USER, .refcnt = 1, .dynamic = 0 } };
pid_namespace_t init_pid_ns = { .common = { .type = NS_TYPE_PID, .refcnt = 1, .dynamic = 0 },
                                .level = 0 };
mnt_namespace_t init_mnt_ns = { .common = { .type = NS_TYPE_MNT, .refcnt = 1, .dynamic = 0 } };
uts_namespace_t init_uts_ns = { .common = { .type = NS_TYPE_UTS, .refcnt = 1, .dynamic = 0 },
                                .hostname = "kx" };
simple_namespace_t init_ipc_ns = { .common = { .type = NS_TYPE_IPC, .refcnt = 1, .dynamic = 0 } };
simple_namespace_t init_net_ns = { .common = { .type = NS_TYPE_NET, .refcnt = 1, .dynamic = 0 } };
simple_namespace_t init_cgroup_ns = { .common = {
                                          .type = NS_TYPE_CGROUP, .refcnt = 1, .dynamic = 0 } };

nsproxy_t init_nsproxy = { .refcnt = 1,
                           .dynamic = 0,
                           .user_ns = &init_user_ns,
                           .pid_ns = &init_pid_ns,
                           .pid_ns_for_children = &init_pid_ns,
                           .mnt_ns = &init_mnt_ns,
                           .uts_ns = &init_uts_ns,
                           .ipc_ns = &init_ipc_ns,
                           .net_ns = &init_net_ns,
                           .cgroup_ns = &init_cgroup_ns };

static void ns_common_get(ns_common_t* ns) {
    if (ns) __atomic_fetch_add(&ns->refcnt, 1, __ATOMIC_RELAXED);
}

static void ns_common_put(ns_common_t* ns);

static void ns_common_free(ns_common_t* ns) {
    if (!ns || !ns->dynamic) return;

    if (ns->type == NS_TYPE_USER) {
        user_namespace_t* u = (user_namespace_t*) ns;
        if (u->parent) ns_common_put(&u->parent->common);
    } else if (ns->type == NS_TYPE_PID) {
        pid_namespace_t* p = (pid_namespace_t*) ns;
        if (p->parent) ns_common_put(&p->parent->common);
    } else if (ns->type == NS_TYPE_MNT) {
        mnt_namespace_t* m = (mnt_namespace_t*) ns;
        for (uint32_t i = 0; i < NS_MNT_MAX; i++) {
            if (m->mounts[i].used && m->mounts[i].root)
                vfs_node_unref_internal(m->mounts[i].root);
        }
        if (m->root) vfs_node_unref_internal(m->root);
    }
    kfree(ns);
}

static void ns_common_put(ns_common_t* ns) {
    if (!ns) return;
    if (__atomic_fetch_sub(&ns->refcnt, 1, __ATOMIC_ACQ_REL) == 1) ns_common_free(ns);
}

static user_namespace_t* user_ns_create(user_namespace_t* parent, uint32_t owner) {
    user_namespace_t* ns = (user_namespace_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->common.type = NS_TYPE_USER;
    ns->common.refcnt = 1;
    ns->common.dynamic = 1;
    ns->parent = parent;
    if (parent) ns_common_get(&parent->common);
    ns->owner = owner;
    return ns;
}

static pid_namespace_t* pid_ns_create(pid_namespace_t* parent) {
    pid_namespace_t* ns = (pid_namespace_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->common.type = NS_TYPE_PID;
    ns->common.refcnt = 1;
    ns->common.dynamic = 1;
    ns->parent = parent;
    if (parent) ns_common_get(&parent->common);
    ns->level = parent ? parent->level + 1 : 1;
    ns->next_pid = 1;
    return ns;
}

mnt_namespace_t* ns_mnt_current(void) {
    mnt_namespace_t* ns = (g_current_proc && g_current_proc->nsproxy)
                              ? g_current_proc->nsproxy->mnt_ns
                              : &init_mnt_ns;
    if (!ns) ns = &init_mnt_ns;
    if (!ns->root) ns->root = vfs_root_node();
    return ns;
}

mnt_namespace_t* ns_mnt_clone(mnt_namespace_t* src) {
    if (!src) src = ns_mnt_current();
    if (!src->root) src->root = vfs_root_node();

    mnt_namespace_t* ns = (mnt_namespace_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->common.type = NS_TYPE_MNT;
    ns->common.refcnt = 1;
    ns->common.dynamic = 1;
    ns->root = src->root;
    ns->root_flags = src->root_flags;
    ns->nmounts = src->nmounts;
    if (ns->root) vfs_node_ref_internal(ns->root);
    for (uint32_t i = 0; i < NS_MNT_MAX; i++) {
        ns->mounts[i] = src->mounts[i];
        if (ns->mounts[i].used && ns->mounts[i].root)
            vfs_node_ref_internal(ns->mounts[i].root);
    }
    return ns;
}

void ns_mnt_put(mnt_namespace_t* nm) {
    if (nm) ns_common_put(&nm->common);
}

void ns_mnt_set(struct proc* p, mnt_namespace_t* nm) {
    if (!p || !p->nsproxy || !nm) return;
    ns_common_get(&nm->common);
    ns_common_put(&p->nsproxy->mnt_ns->common);
    p->nsproxy->mnt_ns = nm;
}

mnt_mount_t* ns_mnt_find(mnt_namespace_t* ns, const char* target) {
    if (!ns || !target) return NULL;
    for (int i = (int) NS_MNT_MAX - 1; i >= 0; i--) {
        if (ns->mounts[i].used && strcmp(ns->mounts[i].target, target) == 0)
            return &ns->mounts[i];
    }
    return NULL;
}

static uts_namespace_t* uts_ns_create(const uts_namespace_t* src) {
    uts_namespace_t* ns = (uts_namespace_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->common.type = NS_TYPE_UTS;
    ns->common.refcnt = 1;
    ns->common.dynamic = 1;
    if (src) {
        memcpy(ns->hostname, src->hostname, sizeof(ns->hostname));
    } else {
        memcpy(ns->hostname, "kx", 3);
    }
    return ns;
}

static simple_namespace_t* simple_ns_create(uint32_t type) {
    simple_namespace_t* ns = (simple_namespace_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->common.type = type;
    ns->common.refcnt = 1;
    ns->common.dynamic = 1;
    return ns;
}

void ns_init(void) {
    init_pid_ns.parent = NULL;
    init_pid_ns.level = 0;
    init_pid_ns.next_pid = NS_PID_MAX + 1;
    for (uint32_t i = 0; i < NS_PID_MAX; i++) {
        init_pid_ns.local_of_global[i] = i + 1;
        init_pid_ns.global_of_local[i] = i + 1;
    }
    log_info("NS: initial namespaces ready");
}

nsproxy_t* nsproxy_get(nsproxy_t* ns) {
    if (ns) __atomic_fetch_add(&ns->refcnt, 1, __ATOMIC_RELAXED);
    return ns;
}

void nsproxy_put(nsproxy_t* ns) {
    if (!ns || !ns->dynamic) return;
    if (__atomic_fetch_sub(&ns->refcnt, 1, __ATOMIC_ACQ_REL) != 1) return;

    ns_common_put(&ns->user_ns->common);
    ns_common_put(&ns->pid_ns->common);
    ns_common_put(&ns->pid_ns_for_children->common);
    ns_common_put(&ns->mnt_ns->common);
    ns_common_put(&ns->uts_ns->common);
    ns_common_put(&ns->ipc_ns->common);
    ns_common_put(&ns->net_ns->common);
    ns_common_put(&ns->cgroup_ns->common);
    kfree(ns);
}

nsproxy_t* nsproxy_clone(const nsproxy_t* src) {
    if (!src) src = &init_nsproxy;
    nsproxy_t* ns = (nsproxy_t*) kmalloc(sizeof(*ns));
    if (!ns) return NULL;
    memset(ns, 0, sizeof(*ns));
    ns->refcnt = 1;
    ns->dynamic = 1;
    ns->user_ns = src->user_ns;
    ns->pid_ns = src->pid_ns;
    ns->pid_ns_for_children = src->pid_ns_for_children;
    ns->mnt_ns = src->mnt_ns;
    ns->uts_ns = src->uts_ns;
    ns->ipc_ns = src->ipc_ns;
    ns->net_ns = src->net_ns;
    ns->cgroup_ns = src->cgroup_ns;
    ns_common_get(&ns->user_ns->common);
    ns_common_get(&ns->pid_ns->common);
    ns_common_get(&ns->pid_ns_for_children->common);
    ns_common_get(&ns->mnt_ns->common);
    ns_common_get(&ns->uts_ns->common);
    ns_common_get(&ns->ipc_ns->common);
    ns_common_get(&ns->net_ns->common);
    ns_common_get(&ns->cgroup_ns->common);
    return ns;
}

static bool pid_ns_register(pid_namespace_t* ns, uint32_t global) {
    if (!ns || global == 0 || global > NS_PID_MAX) return false;
    if (ns == &init_pid_ns) return true;
    if (!pid_ns_register(ns->parent, global)) return false;
    if (ns->local_of_global[global - 1]) return true;

    uint32_t local = ns->next_pid++;
    if (local == 0 || local > NS_PID_MAX) {
        ns->next_pid = NS_PID_MAX + 1;
        if (ns->parent) {
            uint32_t plocal = ns->parent->local_of_global[global - 1];
            if (plocal) ns->parent->local_of_global[global - 1] = 0;
            if (plocal) ns->parent->global_of_local[plocal - 1] = 0;
        }
        return false;
    }
    ns->local_of_global[global - 1] = local;
    ns->global_of_local[local - 1] = global;
    return true;
}

static void pid_ns_unregister(pid_namespace_t* ns, uint32_t global) {
    if (!ns || ns == &init_pid_ns || global == 0 || global > NS_PID_MAX) return;
    uint32_t local = ns->local_of_global[global - 1];
    if (!local) return;
    ns->local_of_global[global - 1] = 0;
    ns->global_of_local[local - 1] = 0;
    pid_ns_unregister(ns->parent, global);
}

void ns_proc_register(struct proc* p) {
    if (!p || !p->nsproxy) return;
    pid_ns_register(p->nsproxy->pid_ns, p->pid);
}

void ns_proc_unregister(struct proc* p) {
    if (!p || !p->nsproxy) return;
    pid_ns_unregister(p->nsproxy->pid_ns, p->pid);
}

static int ns_apply_new_namespaces(nsproxy_t* np, struct proc* owner, uint64_t flags) {
    if (flags & CLONE_NEWUSER) {
        user_namespace_t* nu = user_ns_create(np->user_ns, owner->euid);
        if (!nu) return -ENOMEM;
        ns_common_put(&np->user_ns->common);
        np->user_ns = nu;
    }
    if (flags & CLONE_NEWNS) {
        mnt_namespace_t* nm = ns_mnt_clone(np->mnt_ns);
        if (!nm) return -ENOMEM;
        ns_common_put(&np->mnt_ns->common);
        np->mnt_ns = nm;
    }
    if (flags & CLONE_NEWUTS) {
        uts_namespace_t* nu = uts_ns_create(np->uts_ns);
        if (!nu) return -ENOMEM;
        ns_common_put(&np->uts_ns->common);
        np->uts_ns = nu;
    }
    if (flags & CLONE_NEWIPC) {
        simple_namespace_t* ni = simple_ns_create(NS_TYPE_IPC);
        if (!ni) return -ENOMEM;
        ns_common_put(&np->ipc_ns->common);
        np->ipc_ns = ni;
    }
    if (flags & CLONE_NEWNET) {
        simple_namespace_t* nn = simple_ns_create(NS_TYPE_NET);
        if (!nn) return -ENOMEM;
        ns_common_put(&np->net_ns->common);
        np->net_ns = nn;
    }
    if (flags & CLONE_NEWCGROUP) {
        simple_namespace_t* nc = simple_ns_create(NS_TYPE_CGROUP);
        if (!nc) return -ENOMEM;
        ns_common_put(&np->cgroup_ns->common);
        np->cgroup_ns = nc;
    }
    if (flags & CLONE_NEWPID) {
        pid_namespace_t* npid = pid_ns_create(np->pid_ns);
        if (!npid) return -ENOMEM;
        ns_common_put(&np->pid_ns->common);
        ns_common_put(&np->pid_ns_for_children->common);
        np->pid_ns = npid;
        ns_common_get(&npid->common);
        np->pid_ns_for_children = npid;
    }
    return 0;
}

int ns_child_prepare(struct proc* parent, struct proc* child, uint64_t clone_flags, bool thread) {
    if (!parent || !child) return -EINVAL;
    if (thread && (clone_flags & NS_CLONE_ALL)) return -EINVAL;

    nsproxy_t* np;
    if (thread) {
        np = nsproxy_get(parent->nsproxy);
    } else {
        np = nsproxy_clone(parent->nsproxy);
        if (!np) return -ENOMEM;

        if (clone_flags & NS_CLONE_ALL) {
            int rc = ns_apply_new_namespaces(np, parent, clone_flags);
            if (rc < 0) {
                nsproxy_put(np);
                return rc;
            }
        }

        if (!(clone_flags & CLONE_NEWPID)) {
            pid_namespace_t* target = parent->nsproxy->pid_ns_for_children;
            if (np->pid_ns != target) {
                ns_common_get(&target->common);
                ns_common_put(&np->pid_ns->common);
                np->pid_ns = target;
            }
            if (np->pid_ns_for_children != target) {
                ns_common_get(&target->common);
                ns_common_put(&np->pid_ns_for_children->common);
                np->pid_ns_for_children = target;
            }
        }
    }

    nsproxy_put(child->nsproxy);
    child->nsproxy = np;
    if (!pid_ns_register(child->nsproxy->pid_ns, child->pid)) {
        return -EAGAIN;
    }
    return 0;
}

int ns_unshare(struct proc* p, uint64_t flags) {
    if (!p) return -EPERM;
    if (flags & ~(uint64_t) NS_CLONE_ALL) return -EINVAL;
    if (!flags) return 0;
    if (p->is_thread && (flags & (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS))) return -EINVAL;
    if (!(flags & CLONE_NEWUSER) && !host_priv() && !ns_user_is_root(p)) return -EPERM;

    nsproxy_t* np = nsproxy_clone(p->nsproxy);
    if (!np) return -ENOMEM;

    if (flags & CLONE_NEWUSER) {
        user_namespace_t* nu = user_ns_create(np->user_ns, p->euid);
        if (!nu) goto fail;
        ns_common_put(&np->user_ns->common);
        np->user_ns = nu;
    }
    if (flags & CLONE_NEWNS) {
        mnt_namespace_t* nm = ns_mnt_clone(np->mnt_ns);
        if (!nm) goto fail;
        ns_common_put(&np->mnt_ns->common);
        np->mnt_ns = nm;
    }
    if (flags & CLONE_NEWUTS) {
        uts_namespace_t* nu = uts_ns_create(np->uts_ns);
        if (!nu) goto fail;
        ns_common_put(&np->uts_ns->common);
        np->uts_ns = nu;
    }
    if (flags & CLONE_NEWIPC) {
        simple_namespace_t* ni = simple_ns_create(NS_TYPE_IPC);
        if (!ni) goto fail;
        ns_common_put(&np->ipc_ns->common);
        np->ipc_ns = ni;
    }
    if (flags & CLONE_NEWNET) {
        simple_namespace_t* nn = simple_ns_create(NS_TYPE_NET);
        if (!nn) goto fail;
        ns_common_put(&np->net_ns->common);
        np->net_ns = nn;
    }
    if (flags & CLONE_NEWCGROUP) {
        simple_namespace_t* nc = simple_ns_create(NS_TYPE_CGROUP);
        if (!nc) goto fail;
        ns_common_put(&np->cgroup_ns->common);
        np->cgroup_ns = nc;
    }
    if (flags & CLONE_NEWPID) {
        pid_namespace_t* npid = pid_ns_create(np->pid_ns);
        if (!npid) goto fail;
        ns_common_put(&np->pid_ns_for_children->common);
        ns_common_get(&npid->common);
        np->pid_ns_for_children = npid;
    }

    nsproxy_t* old = p->nsproxy;
    p->nsproxy = np;
    nsproxy_put(old);
    return 0;

fail:
    nsproxy_put(np);
    return -ENOMEM;
}

const char* ns_type_name(uint32_t type) {
    switch (type) {
    case NS_TYPE_USER:
        return "user";
    case NS_TYPE_PID:
        return "pid";
    case NS_TYPE_MNT:
        return "mnt";
    case NS_TYPE_UTS:
        return "uts";
    case NS_TYPE_IPC:
        return "ipc";
    case NS_TYPE_NET:
        return "net";
    case NS_TYPE_CGROUP:
        return "cgroup";
    default:
        return "?";
    }
}

ns_common_t* ns_for_proc(const struct proc* p, uint32_t type, bool for_children) {
    if (!p || !p->nsproxy) return NULL;
    switch (type) {
    case NS_TYPE_USER:
        return &p->nsproxy->user_ns->common;
    case NS_TYPE_PID:
        return for_children ? &p->nsproxy->pid_ns_for_children->common
                            : &p->nsproxy->pid_ns->common;
    case NS_TYPE_MNT:
        return &p->nsproxy->mnt_ns->common;
    case NS_TYPE_UTS:
        return &p->nsproxy->uts_ns->common;
    case NS_TYPE_IPC:
        return &p->nsproxy->ipc_ns->common;
    case NS_TYPE_NET:
        return &p->nsproxy->net_ns->common;
    case NS_TYPE_CGROUP:
        return &p->nsproxy->cgroup_ns->common;
    default:
        return NULL;
    }
}

uint32_t ns_proc_pid_in(const struct proc* p, const pid_namespace_t* ns) {
    if (!p || !ns || p->pid == 0 || p->pid > NS_PID_MAX) return 0;
    if (ns == &init_pid_ns) return p->pid;
    return ns->local_of_global[p->pid - 1];
}

uint32_t ns_proc_pid(const struct proc* p) {
    if (!p || !p->nsproxy) return p ? p->pid : 0;
    return ns_proc_pid_in(p, p->nsproxy->pid_ns);
}

uint32_t ns_pid_to_global(const pid_namespace_t* ns, uint32_t local) {
    if (!ns || local == 0 || local > NS_PID_MAX) return 0;
    if (ns == &init_pid_ns) return local;
    return ns->global_of_local[local - 1];
}

int ns_pid_chain(const struct proc* p, uint32_t* out, int max) {
    if (!p || !p->nsproxy || !out || max <= 0) return 0;
    int n = 0;
    for (pid_namespace_t* ns = p->nsproxy->pid_ns; ns && n < max; ns = ns->parent) {
        uint32_t local = ns_proc_pid_in(p, ns);
        if (!local) break;
        out[n++] = local;
    }
    return n;
}

int ns_uid_to_local(const user_namespace_t* ns, uint32_t kuid, uint32_t* out) {
    if (out) *out = 65534;
    if (!ns || ns == &init_user_ns) {
        if (out) *out = kuid;
        return 0;
    }
    for (int i = 0; i < ns->nuid_map; i++) {
        uint64_t off = (uint64_t) kuid - ns->uid_map[i].outside;
        if (kuid >= ns->uid_map[i].outside && off < ns->uid_map[i].count) {
            if (out) *out = ns->uid_map[i].inside + (uint32_t) off;
            return 0;
        }
    }
    return -1;
}

int ns_gid_to_local(const user_namespace_t* ns, uint32_t kgid, uint32_t* out) {
    if (out) *out = 65534;
    if (!ns || ns == &init_user_ns) {
        if (out) *out = kgid;
        return 0;
    }
    for (int i = 0; i < ns->ngid_map; i++) {
        uint64_t off = (uint64_t) kgid - ns->gid_map[i].outside;
        if (kgid >= ns->gid_map[i].outside && off < ns->gid_map[i].count) {
            if (out) *out = ns->gid_map[i].inside + (uint32_t) off;
            return 0;
        }
    }
    return -1;
}

int ns_local_to_kuid(const user_namespace_t* ns, uint32_t uid, uint32_t* out) {
    if (!ns || ns == &init_user_ns) {
        if (out) *out = uid;
        return 0;
    }
    for (int i = 0; i < ns->nuid_map; i++) {
        uint64_t off = (uint64_t) uid - ns->uid_map[i].inside;
        if (uid >= ns->uid_map[i].inside && off < ns->uid_map[i].count) {
            if (out) *out = ns->uid_map[i].outside + (uint32_t) off;
            return 0;
        }
    }
    return -1;
}

int ns_local_to_kgid(const user_namespace_t* ns, uint32_t gid, uint32_t* out) {
    if (!ns || ns == &init_user_ns) {
        if (out) *out = gid;
        return 0;
    }
    for (int i = 0; i < ns->ngid_map; i++) {
        uint64_t off = (uint64_t) gid - ns->gid_map[i].inside;
        if (gid >= ns->gid_map[i].inside && off < ns->gid_map[i].count) {
            if (out) *out = ns->gid_map[i].outside + (uint32_t) off;
            return 0;
        }
    }
    return -1;
}

bool ns_user_is_initial(const struct proc* p) {
    return !p || !p->nsproxy || p->nsproxy->user_ns == &init_user_ns;
}

bool ns_user_is_root(const struct proc* p) {
    if (!p || !p->nsproxy) return false;
    uint32_t local = 65534;
    ns_uid_to_local(p->nsproxy->user_ns, p->euid, &local);
    return local == 0;
}

const char* ns_uts_hostname(const struct proc* p) {
    if (!p || !p->nsproxy || !p->nsproxy->uts_ns) return "kx";
    return p->nsproxy->uts_ns->hostname;
}

int ns_uts_sethostname(struct proc* p, const char* name, uint64_t len) {
    if (!p || !p->nsproxy || !name) return -EINVAL;
    if (len == 0 || len > 64) return -EINVAL;
    memcpy(p->nsproxy->uts_ns->hostname, name, len);
    p->nsproxy->uts_ns->hostname[len] = '\0';
    return 0;
}

static bool parse_u32_span(const char** sp, const char* end, uint32_t* out) {
    const char* s = *sp;
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    if (s >= end || *s < '0' || *s > '9') return false;
    uint64_t v = 0;
    while (s < end && *s >= '0' && *s <= '9') {
        v = v * 10 + (uint64_t) (*s - '0');
        if (v > 0xffffffffULL) return false;
        s++;
    }
    while (s < end && (*s == ' ' || *s == '\t')) s++;
    *sp = s;
    *out = (uint32_t) v;
    return true;
}

static int user_map_write(user_namespace_t* ns, bool gid, const char* buf, uint64_t len) {
    char tmp[512];
    if (!buf || len == 0 || len >= sizeof(tmp)) return -EINVAL;
    if (!uptr_ok(buf, len)) return -EFAULT;
    memcpy(tmp, buf, len);
    tmp[len] = '\0';

    if ((gid && ns->gid_map_written) || (!gid && ns->map_written)) return -EPERM;
    if (gid && !ns->groups_disabled) return -EPERM;

    ns_uid_gid_map_t maps[NS_UID_GID_MAP_MAX];
    memset(maps, 0, sizeof(maps));
    int count = 0;
    const char* s = tmp;
    const char* end = tmp + len;

    while (s < end) {
        while (s < end && (*s == '\n' || *s == '\r' || *s == ' ' || *s == '\t')) s++;
        if (s >= end) break;
        if (count >= NS_UID_GID_MAP_MAX) return -EINVAL;

        uint32_t inside, outside, n;
        if (!parse_u32_span(&s, end, &inside)) return -EINVAL;
        if (!parse_u32_span(&s, end, &outside)) return -EINVAL;
        if (!parse_u32_span(&s, end, &n)) return -EINVAL;
        if (n == 0) return -EINVAL;
        if ((uint64_t) inside + n > 0x100000000ULL) return -EINVAL;
        if ((uint64_t) outside + n > 0x100000000ULL) return -EINVAL;

        for (int i = 0; i < count; i++) {
            uint64_t a1 = inside, a2 = (uint64_t) inside + n;
            uint64_t b1 = maps[i].inside, b2 = (uint64_t) maps[i].inside + maps[i].count;
            if (a1 < b2 && b1 < a2) return -EINVAL;
            a1 = outside;
            a2 = (uint64_t) outside + n;
            b1 = maps[i].outside;
            b2 = (uint64_t) maps[i].outside + maps[i].count;
            if (a1 < b2 && b1 < a2) return -EINVAL;
        }
        maps[count].inside = inside;
        maps[count].outside = outside;
        maps[count].count = n;
        count++;

        while (s < end && (*s == ' ' || *s == '\t')) s++;
        if (s < end && *s != '\n' && *s != '\r') return -EINVAL;
    }

    if (count == 0) return -EINVAL;
    if (gid) {
        memcpy(ns->gid_map, maps, sizeof(maps[0]) * (size_t) count);
        ns->ngid_map = (uint8_t) count;
        ns->gid_map_written = 1;
    } else {
        memcpy(ns->uid_map, maps, sizeof(maps[0]) * (size_t) count);
        ns->nuid_map = (uint8_t) count;
        ns->map_written = 1;
    }
    return 0;
}

int ns_user_map_read(struct proc* target, bool gid, char* buf, uint64_t len) {
    if (!target || !target->nsproxy || !buf) return -EINVAL;
    user_namespace_t* ns = target->nsproxy->user_ns;
    if (ns == &init_user_ns) {
        const char* s = "0 0 4294967295\n";
        uint64_t n = strlen(s);
        if (n > len) n = len;
        memcpy(buf, s, n);
        return (int) n;
    }

    int used = 0;
    int count = gid ? ns->ngid_map : ns->nuid_map;
    ns_uid_gid_map_t* maps = gid ? ns->gid_map : ns->uid_map;
    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + used, (size_t) (len - (uint64_t) used), "%u %u %u\n", maps[i].inside,
                         maps[i].outside, maps[i].count);
        if (n < 0 || (uint64_t) n >= len - (uint64_t) used) break;
        used += n;
    }
    return used;
}

int ns_user_map_write(struct proc* target, bool gid, const char* buf, uint64_t len) {
    if (!target || !target->nsproxy) return -EINVAL;
    user_namespace_t* ns = target->nsproxy->user_ns;
    if (ns == &init_user_ns) return -EPERM;
    if (target != cur() && !host_priv()) return -EPERM;
    return user_map_write(ns, gid, buf, len);
}

int ns_user_setgroups_read(struct proc* target, char* buf, uint64_t len) {
    if (!target || !target->nsproxy || !buf) return -EINVAL;
    user_namespace_t* ns = target->nsproxy->user_ns;
    const char* s = (ns != &init_user_ns && ns->groups_disabled) ? "deny\n" : "allow\n";
    uint64_t n = strlen(s);
    if (n > len) n = len;
    memcpy(buf, s, n);
    return (int) n;
}

int ns_user_setgroups_write(struct proc* target, const char* buf, uint64_t len) {
    if (!target || !target->nsproxy || !buf) return -EINVAL;
    user_namespace_t* ns = target->nsproxy->user_ns;
    if (ns == &init_user_ns) return -EPERM;
    if (target != cur() && !host_priv()) return -EPERM;
    if (ns->gid_map_written) return -EPERM;
    char tmp[16];
    if (len == 0 || len >= sizeof(tmp)) return -EINVAL;
    if (!uptr_ok(buf, len)) return -EFAULT;
    memcpy(tmp, buf, len);
    tmp[len] = '\0';
    if (len >= 4 && memcmp(tmp, "deny", 4) == 0) {
        ns->groups_disabled = 1;
        return 0;
    }
    if (len >= 5 && memcmp(tmp, "allow", 5) == 0) {
        ns->groups_disabled = 0;
        return 0;
    }
    return -EINVAL;
}

static int type_flag(uint32_t type, bool for_children) {
    switch (type) {
    case NS_TYPE_USER:
        return CLONE_NEWUSER;
    case NS_TYPE_PID:
        return CLONE_NEWPID;
    case NS_TYPE_MNT:
        return CLONE_NEWNS;
    case NS_TYPE_UTS:
        return CLONE_NEWUTS;
    case NS_TYPE_IPC:
        return CLONE_NEWIPC;
    case NS_TYPE_NET:
        return CLONE_NEWNET;
    case NS_TYPE_CGROUP:
        return CLONE_NEWCGROUP;
    default:
        (void) for_children;
        return -1;
    }
}

static bool parse_ns_path(const char* path, bool* self, uint32_t* pid, uint32_t* type,
                          bool* for_children) {
    if (!path || strncmp(path, "/proc/", 6) != 0) return false;
    const char* s = path + 6;
    *self = false;
    *pid = 0;
    if (strncmp(s, "self/ns/", 8) == 0) {
        *self = true;
        s += 8;
    } else {
        while (*s >= '0' && *s <= '9') {
            *pid = *pid * 10 + (uint32_t) (*s - '0');
            s++;
        }
        if (s == path + 6 || strncmp(s, "/ns/", 4) != 0) return false;
        s += 4;
    }

    *for_children = false;
    if (strcmp(s, "user") == 0) {
        *type = NS_TYPE_USER;
    } else if (strcmp(s, "pid") == 0) {
        *type = NS_TYPE_PID;
    } else if (strcmp(s, "pid_for_children") == 0) {
        *type = NS_TYPE_PID;
        *for_children = true;
    } else if (strcmp(s, "mnt") == 0) {
        *type = NS_TYPE_MNT;
    } else if (strcmp(s, "uts") == 0) {
        *type = NS_TYPE_UTS;
    } else if (strcmp(s, "ipc") == 0) {
        *type = NS_TYPE_IPC;
    } else if (strcmp(s, "net") == 0) {
        *type = NS_TYPE_NET;
    } else if (strcmp(s, "cgroup") == 0) {
        *type = NS_TYPE_CGROUP;
    } else {
        return false;
    }
    return true;
}

int ns_setns(struct proc* p, struct vfs_node* node, int nstype) {
    if (!p || !node) return -EINVAL;
    char path[512];
    if (!vfs_node_abspath(node, path, sizeof(path))) return -EINVAL;

    bool self = false, for_children = false;
    uint32_t pid = 0, type = 0;
    if (!parse_ns_path(path, &self, &pid, &type, &for_children)) return -EINVAL;

    int expected = type_flag(type, for_children);
    if (expected < 0) return -EINVAL;
    if (nstype && nstype != expected) return -EINVAL;

    proc_t* target = NULL;
    if (self) {
        target = p;
    } else {
        target = proc_find(pid);
        if (!target) return -ESRCH;
    }

    if (!host_priv() && !ns_user_is_root(p)) {
        if (!self) proc_unref(target);
        return -EPERM;
    }

    ns_common_t* src = ns_for_proc(target, type, for_children);
    if (!src) {
        if (!self) proc_unref(target);
        return -EINVAL;
    }

    if (type == NS_TYPE_PID) {
        pid_namespace_t* npid = (pid_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->pid_ns_for_children->common);
        p->nsproxy->pid_ns_for_children = npid;
    } else if (type == NS_TYPE_USER) {
        if (p->is_thread) {
            if (!self) proc_unref(target);
            return -EINVAL;
        }
        user_namespace_t* nu = (user_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->user_ns->common);
        p->nsproxy->user_ns = nu;
    } else if (type == NS_TYPE_MNT) {
        mnt_namespace_t* nm = (mnt_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->mnt_ns->common);
        p->nsproxy->mnt_ns = nm;
    } else if (type == NS_TYPE_UTS) {
        uts_namespace_t* nu = (uts_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->uts_ns->common);
        p->nsproxy->uts_ns = nu;
    } else if (type == NS_TYPE_IPC) {
        simple_namespace_t* ni = (simple_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->ipc_ns->common);
        p->nsproxy->ipc_ns = ni;
    } else if (type == NS_TYPE_NET) {
        simple_namespace_t* nn = (simple_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->net_ns->common);
        p->nsproxy->net_ns = nn;
    } else if (type == NS_TYPE_CGROUP) {
        simple_namespace_t* nc = (simple_namespace_t*) src;
        ns_common_get(src);
        ns_common_put(&p->nsproxy->cgroup_ns->common);
        p->nsproxy->cgroup_ns = nc;
    }

    if (!self) proc_unref(target);
    return 0;
}

int64_t sys_unshare(uint64_t flags) {
    return (int64_t) ns_unshare(cur(), flags);
}

int64_t sys_setns(int fd, int nstype) {
    vfs_file_t* f = fd_get_file(fd);
    if (!f || !f->node) return -(int64_t) EINVAL;
    return (int64_t) ns_setns(cur(), f->node, nstype);
}
