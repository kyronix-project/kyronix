#include "cred.h"

#include "fs/vfs.h"
#include "fs/vfs_internal.h"
#include "internal.h"
#include "lib/string.h"
#include "proc/jail.h"
#include "proc/ns.h"
#include "proc/proc.h"
#include "security/caps.h"

static void kernel_initgroups(proc_t *p, uint32_t uid);

static user_namespace_t *cred_userns(proc_t *p) {
    return (p && p->nsproxy) ? p->nsproxy->user_ns : &init_user_ns;
}

static uint32_t cred_uid(proc_t *p, uint32_t kuid) {
    uint32_t out = 65534;
    ns_uid_to_local(cred_userns(p), kuid, &out);
    return out;
}

static uint32_t cred_gid(proc_t *p, uint32_t kgid) {
    uint32_t out = 65534;
    ns_gid_to_local(cred_userns(p), kgid, &out);
    return out;
}

#define CAP_VERSION_3 0x20080522U

typedef struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
} cap_data_t;

typedef struct {
    uint32_t version;
    int pid;
} cap_header_t;

bool cred_has_cap(proc_t *p, uint32_t cap) {
    if (!p || cap > CAP_LAST_CAP) return false;
    return (p->cap_effective & (1ULL << cap)) != 0;
}

void cred_drop_caps_for_uid(proc_t *p, uint32_t old_euid, uint32_t new_euid) {
    if (!p || old_euid != 0 || new_euid == 0 || p->keepcaps) return;
    p->cap_effective = 0;
    p->cap_permitted = 0;
    p->cap_ambient = 0;
}

int64_t sys_capget(void *uheader, void *udata) {
    if (!uptr_ok(uheader, sizeof(cap_header_t)) || !uptr_ok_w(uheader, sizeof(cap_header_t)))
        return -(int64_t) EFAULT;

    cap_header_t header = *(cap_header_t *) uheader;
    if (header.version != CAP_VERSION_3) {
        header.version = CAP_VERSION_3;
        *(cap_header_t *) uheader = header;
        return -(int64_t) EINVAL;
    }

    proc_t *p = cur();
    if (!p) return -(int64_t) ESRCH;
    if (header.pid != 0 && header.pid != (int) sys_getpid()) return -(int64_t) EPERM;
    if (!uptr_ok_w(udata, 2 * sizeof(cap_data_t))) return -(int64_t) EFAULT;

    cap_data_t *data = (cap_data_t *) udata;
    data[0].effective = (uint32_t) p->cap_effective;
    data[0].permitted = (uint32_t) p->cap_permitted;
    data[0].inheritable = (uint32_t) p->cap_inheritable;
    data[1].effective = (uint32_t) (p->cap_effective >> 32);
    data[1].permitted = (uint32_t) (p->cap_permitted >> 32);
    data[1].inheritable = (uint32_t) (p->cap_inheritable >> 32);
    return 0;
}

int64_t sys_capset(void *uheader, const void *udata) {
    if (!uptr_ok(uheader, sizeof(cap_header_t)) || !uptr_ok(udata, 2 * sizeof(cap_data_t)))
        return -(int64_t) EFAULT;

    cap_header_t header = *(cap_header_t *) uheader;
    if (header.version != CAP_VERSION_3) {
        header.version = CAP_VERSION_3;
        *(cap_header_t *) uheader = header;
        return -(int64_t) EINVAL;
    }
    if (header.pid != 0 && header.pid != (int) sys_getpid()) return -(int64_t) EPERM;

    const cap_data_t *data = (const cap_data_t *) udata;
    uint64_t effective = (uint64_t) data[0].effective | ((uint64_t) data[1].effective << 32);
    uint64_t permitted = (uint64_t) data[0].permitted | ((uint64_t) data[1].permitted << 32);
    uint64_t inheritable = (uint64_t) data[0].inheritable | ((uint64_t) data[1].inheritable << 32);
    proc_t *p = cur();
    if (!p) return -(int64_t) ESRCH;

    if ((effective & ~permitted) != 0) return -(int64_t) EPERM;
    if ((permitted & ~p->cap_permitted) != 0) return -(int64_t) EPERM;
    if ((inheritable & ~(p->cap_inheritable | p->cap_permitted)) != 0)
        return -(int64_t) EPERM;

    p->cap_effective = effective & CAP_FULL_MASK;
    p->cap_permitted = permitted & CAP_FULL_MASK;
    p->cap_inheritable = inheritable & CAP_FULL_MASK;
    p->cap_ambient &= p->cap_inheritable & p->cap_permitted;
    return 0;
}

int64_t sys_getpid(void) {
    proc_t *p = cur();
    if (!p) return 1;
    uint32_t pid = ns_proc_pid(p);
    return pid ? (int64_t) pid : 1;
}

int64_t sys_getppid(void) {
    proc_t *p = cur();
    if (!p || !p->ppid) return 0;
    proc_t *parent = proc_find(p->ppid);
    if (!parent) return 0;
    uint32_t pid = ns_proc_pid_in(parent, p->nsproxy ? p->nsproxy->pid_ns : &init_pid_ns);
    proc_unref(parent);
    return (int64_t) pid;
}

int64_t sys_getuid(void) {
    proc_t *p = cur();
    return p ? (int64_t) cred_uid(p, p->uid) : 0;
}
int64_t sys_getgid(void) {
    proc_t *p = cur();
    return p ? (int64_t) cred_gid(p, p->gid) : 0;
}
int64_t sys_geteuid(void) {
    proc_t *p = cur();
    return p ? (int64_t) cred_uid(p, p->euid) : 0;
}
int64_t sys_getegid(void) {
    proc_t *p = cur();
    return p ? (int64_t) cred_gid(p, p->egid) : 0;
}

bool is_root(void) {
    proc_t *p = cur();
    return p && p->euid == 0;
}

// host-global privilege: euid 0 AND not confined by a JAILF_PRIV jail
bool host_priv(void) { return jail_host_priv(cur()); }

int64_t sys_setfsuid(uint32_t uid) {
    proc_t *p = cur();
    if (!p) return 0;
    uint32_t old = p->fsuid;
    if (uid == (uint32_t) -1) return old;
    if (ns_user_is_root(p) || uid == p->uid || uid == p->euid || uid == p->suid) p->fsuid = uid;
    return old;
}

int64_t sys_setfsgid(uint32_t gid) {
    proc_t *p = cur();
    if (!p) return 0;
    uint32_t old = p->fsgid;
    if (gid == (uint32_t) -1) return old;
    if (ns_user_is_root(p) || gid == p->gid || gid == p->egid || gid == p->sgid) p->fsgid = gid;
    return old;
}

int64_t sys_setuid(uint32_t uid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (ns_user_is_root(p)) {
        uint32_t old_euid = p->euid;
        p->uid = p->euid = p->suid = p->fsuid = uid;
        cred_drop_caps_for_uid(p, old_euid, p->euid);
        if (uid != 0) kernel_initgroups(p, uid);
        return 0;
    }
    if (uid != p->uid && uid != p->suid) return -(int64_t) EPERM;
    uint32_t old_euid = p->euid;
    p->euid = p->fsuid = uid;
    cred_drop_caps_for_uid(p, old_euid, p->euid);
    return 0;
}
int64_t sys_setgid(uint32_t gid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (ns_user_is_root(p)) {
        p->gid = p->egid = p->sgid = p->fsgid = gid;
        return 0;
    }
    if (gid != p->gid && gid != p->sgid) return -(int64_t) EPERM;
    p->egid = p->fsgid = gid;
    return 0;
}
int64_t sys_setreuid(uint32_t ruid, uint32_t euid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (!ns_user_is_root(p) && (ruid != (uint32_t) -1 && ruid != p->uid && ruid != p->euid))
        return -(int64_t) EPERM;
    if (!ns_user_is_root(p) &&
        (euid != (uint32_t) -1 && euid != p->uid && euid != p->euid && euid != p->suid))
        return -(int64_t) EPERM;
    if (ruid != (uint32_t) -1) p->uid = ruid;
    uint32_t old_euid = p->euid;
    if (euid != (uint32_t) -1) p->euid = p->fsuid = euid;
    cred_drop_caps_for_uid(p, old_euid, p->euid);
    p->suid = p->euid;
    return 0;
}
int64_t sys_setregid(uint32_t rgid, uint32_t egid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (!ns_user_is_root(p) && (rgid != (uint32_t) -1 && rgid != p->gid && rgid != p->egid))
        return -(int64_t) EPERM;
    if (!ns_user_is_root(p) &&
        (egid != (uint32_t) -1 && egid != p->gid && egid != p->egid && egid != p->sgid))
        return -(int64_t) EPERM;
    if (rgid != (uint32_t) -1) p->gid = rgid;
    if (egid != (uint32_t) -1) p->egid = p->fsgid = egid;
    p->sgid = p->egid;
    return 0;
}
int64_t sys_setresuid(uint32_t ruid, uint32_t euid, uint32_t suid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (!ns_user_is_root(p)) {
        uint32_t cur_set[3] = { p->uid, p->euid, p->suid };
        if (ruid != (uint32_t) -1 && ruid != cur_set[0] && ruid != cur_set[1] && ruid != cur_set[2])
            return -(int64_t) EPERM;
        if (euid != (uint32_t) -1 && euid != cur_set[0] && euid != cur_set[1] && euid != cur_set[2])
            return -(int64_t) EPERM;
        if (suid != (uint32_t) -1 && suid != cur_set[0] && suid != cur_set[1] && suid != cur_set[2])
            return -(int64_t) EPERM;
    }
    if (ruid != (uint32_t) -1) p->uid = ruid;
    uint32_t old_euid = p->euid;
    if (euid != (uint32_t) -1) p->euid = p->fsuid = euid;
    cred_drop_caps_for_uid(p, old_euid, p->euid);
    if (suid != (uint32_t) -1) p->suid = suid;
    return 0;
}
int64_t sys_setresgid(uint32_t rgid, uint32_t egid, uint32_t sgid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (!ns_user_is_root(p)) {
        uint32_t cur_set[3] = { p->gid, p->egid, p->sgid };
        if (rgid != (uint32_t) -1 && rgid != cur_set[0] && rgid != cur_set[1] && rgid != cur_set[2])
            return -(int64_t) EPERM;
        if (egid != (uint32_t) -1 && egid != cur_set[0] && egid != cur_set[1] && egid != cur_set[2])
            return -(int64_t) EPERM;
        if (sgid != (uint32_t) -1 && sgid != cur_set[0] && sgid != cur_set[1] && sgid != cur_set[2])
            return -(int64_t) EPERM;
    }
    if (rgid != (uint32_t) -1) p->gid = rgid;
    if (egid != (uint32_t) -1) p->egid = p->fsgid = egid;
    if (sgid != (uint32_t) -1) p->sgid = sgid;
    return 0;
}
int64_t sys_getresuid(uint32_t *ruid, uint32_t *euid, uint32_t *suid) {
    proc_t *p = cur();
    if (!p || !uptr_ok_w(ruid, 4) || !uptr_ok_w(euid, 4) || !uptr_ok_w(suid, 4))
        return -(int64_t) EFAULT;
    *ruid = cred_uid(p, p->uid);
    *euid = cred_uid(p, p->euid);
    *suid = cred_uid(p, p->suid);
    return 0;
}
int64_t sys_getresgid(uint32_t *rgid, uint32_t *egid, uint32_t *sgid) {
    proc_t *p = cur();
    if (!p || !uptr_ok_w(rgid, 4) || !uptr_ok_w(egid, 4) || !uptr_ok_w(sgid, 4))
        return -(int64_t) EFAULT;
    *rgid = cred_gid(p, p->gid);
    *egid = cred_gid(p, p->egid);
    *sgid = cred_gid(p, p->sgid);
    return 0;
}
static int64_t pgid_local(proc_t *self, proc_t *p) {
    if (!self || !p) return 0;
    pid_namespace_t *ns = self->nsproxy ? self->nsproxy->pid_ns : &init_pid_ns;
    proc_t *leader = proc_find((uint32_t) p->pgid);
    if (!leader) return (int64_t) ns_proc_pid_in(p, ns);
    int64_t local = (int64_t) ns_proc_pid_in(leader, ns);
    proc_unref(leader);
    return local;
}

int64_t sys_getpgrp(void) {
    proc_t *p = cur();
    if (!p) return 1;
    int64_t pgid = pgid_local(p, p);
    return pgid ? pgid : (int64_t) ns_proc_pid(p);
}

int64_t sys_getpgid(uint64_t pid) {
    proc_t *self = cur();
    if (!self) return -(int64_t) ESRCH;
    if (pid == 0) return sys_getpgrp();
    uint32_t global = ns_pid_to_global(self->nsproxy ? self->nsproxy->pid_ns : &init_pid_ns,
                                       (uint32_t) pid);
    if (!global) return -(int64_t) ESRCH;
    proc_t *p = proc_find(global);
    if (!p || !jail_can_see(self, p)) {
        if (p) proc_unref(p);
        return -(int64_t) ESRCH;
    }
    int64_t pgid = pgid_local(self, p);
    proc_unref(p);
    return pgid;
}
int64_t sys_setsid(void) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    p->pgid = p->pid;
    return (int64_t) ns_proc_pid(p);
}
int64_t sys_setpgid(uint64_t pid, uint64_t pgid) {
    proc_t *self = cur();
    if (!self) return -(int64_t) ESRCH;
    pid_namespace_t *ns = self->nsproxy ? self->nsproxy->pid_ns : &init_pid_ns;
    uint32_t global_pid = pid ? ns_pid_to_global(ns, (uint32_t) pid) : self->pid;
    if (!global_pid) return -(int64_t) ESRCH;
    proc_t *p = pid ? proc_find(global_pid) : self;
    if (!p) return -(int64_t) ESRCH;
    if (!jail_can_see(self, p)) {
        if (pid) proc_unref(p);
        return -(int64_t) ESRCH;
    }
    if (p != self && (!self || p->ppid != self->pid || p->jail_id != self->jail_id ||
                      (!host_priv() && p->uid != self->uid))) {
        if (pid) proc_unref(p);
        return -(int64_t) EPERM;
    }
    uint32_t global_pgid = p->pid;
    if (pgid != 0) {
        global_pgid = ns_pid_to_global(ns, (uint32_t) pgid);
        if (!global_pgid) {
            if (pid) proc_unref(p);
            return -(int64_t) ESRCH;
        }
        proc_t *leader = proc_find(global_pgid);
        if (!leader) {
            if (pid) proc_unref(p);
            return -(int64_t) ESRCH;
        }
        proc_unref(leader);
    }
    if (global_pgid > PROC_MAX) {
        if (pid) proc_unref(p);
        return -(int64_t) EINVAL;
    }
    p->pgid = (int) global_pgid;
    if (pid) proc_unref(p);
    return 0;
}

int64_t sys_gettid(void) { return sys_getpid(); }

static int parse_col(const char *line, uint64_t linelen, int col, char *out, int outsz) {
    int c = 0;
    const char *start = line;
    for (uint64_t i = 0; i <= linelen; i++) {
        if (i == linelen || line[i] == ':') {
            if (c == col) {
                int len = (int) (line + i - start);
                if (len >= outsz) len = outsz - 1;
                memcpy(out, start, len);
                out[len] = '\0';
                return len;
            }
            c++;
            start = line + i + 1;
        }
    }
    out[0] = '\0';
    return 0;
}

static bool name_in_list(const char *members, const char *name) {
    int nlen = (int) strlen(name);
    const char *p = members;
    while (*p) {
        if (strncmp(p, name, nlen) == 0 && (p[nlen] == ',' || p[nlen] == '\0')) return true;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }
    return false;
}

static void kernel_initgroups(proc_t *p, uint32_t uid) {
    char uname[64] = "";
    uname[0] = '\0';

    vfs_node_t *pw = vfs_lookup("/etc/passwd");
    if (pw && pw->type == VFS_TYPE_REG && pw->data && pw->size > 0) {
        char *data = (char *) pw->data;
        uint64_t sz = pw->size;
        uint64_t pos = 0;
        while (pos < sz) {
            uint64_t linelen = 0;
            while (pos + linelen < sz && data[pos + linelen] != '\n') linelen++;
            char fuid[32] = "";
            parse_col(data + pos, linelen, 2, fuid, sizeof(fuid));
            if (fuid[0] && (uint32_t) atoi(fuid) == uid) {
                parse_col(data + pos, linelen, 0, uname, sizeof(uname));
                break;
            }
            pos += linelen + 1;
        }
        vfs_node_unref_internal(pw);
    }

    uint32_t groups[64];
    int ngroups = 0;
    groups[ngroups++] = p->gid;

    if (!uname[0]) {
        p->ngroups = ngroups;
        return;
    }

    vfs_node_t *gr = vfs_lookup("/etc/group");
    if (gr && gr->type == VFS_TYPE_REG && gr->data && gr->size > 0) {
        char *data = (char *) gr->data;
        uint64_t sz = gr->size;
        uint64_t pos = 0;
        while (pos < sz && ngroups < 64) {
            uint64_t linelen = 0;
            while (pos + linelen < sz && data[pos + linelen] != '\n') linelen++;
            char ggid[32] = "";
            char members[512] = "";
            parse_col(data + pos, linelen, 2, ggid, sizeof(ggid));
            parse_col(data + pos, linelen, 3, members, sizeof(members));
            if (ggid[0] && members[0] && name_in_list(members, uname)) {
                groups[ngroups++] = (uint32_t) atoi(ggid);
            }
            pos += linelen + 1;
        }
        vfs_node_unref_internal(gr);
    }

    p->ngroups = ngroups;
    for (int i = 0; i < ngroups; i++) p->sup_groups[i] = groups[i];
}

int64_t sys_getgroups(int size, uint32_t *list) {
    proc_t *p = cur();
    if (!p) return 0;
    int n = p->ngroups;
    if (size == 0) return n;
    if (size < n) return -(int64_t) EINVAL;
    if (!list || !uptr_ok_w(list, (uint64_t) n * sizeof(*list))) return -(int64_t) EFAULT;
    for (int i = 0; i < n; i++) list[i] = cred_gid(p, p->sup_groups[i]);
    return n;
}

int64_t sys_setgroups(int size, const uint32_t *list) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EPERM;
    if (!ns_user_is_root(p)) return -(int64_t) EPERM;
    if (!ns_user_is_initial(p) && p->nsproxy->user_ns->groups_disabled)
        return -(int64_t) EPERM;
    if (size < 0) return -(int64_t) EINVAL;
    if (size > 64) return -(int64_t) EINVAL;
    if (size > 0 && (!list || !uptr_ok(list, (uint64_t) size * sizeof(*list))))
        return -(int64_t) EFAULT;
    p->ngroups = size;
    for (int i = 0; i < size; i++) p->sup_groups[i] = list[i];
    return 0;
}
