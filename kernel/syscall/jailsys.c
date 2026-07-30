#include "jailsys.h"

#include "internal.h"
#include "lib/string.h"
#include "proc/jail.h"
#include "proc/proc.h"

int64_t sys_jail_create(const kjail_conf_t *ucfg) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    if (!jail_host_priv(p)) return -(int64_t) EPERM; // jail creation is privileged
    if (!ucfg || !uptr_ok(ucfg, sizeof(kjail_conf_t))) return -(int64_t) EFAULT;
    kjail_conf_t cfg;
    memcpy(&cfg, ucfg, sizeof(cfg));
    cfg.name[JAIL_NAME_MAX - 1] = '\0';
    cfg.root[JAIL_ROOT_MAX - 1] = '\0';
    int jid = jail_create(p->jail_id, &cfg, p->euid);
    if (jid < 0) return jid;
    if (cfg.attach) jail_enter(p, (uint32_t) jid);
    return jid;
}

int64_t sys_jail_attach(uint32_t jid) {
    proc_t *p = cur();
    if (!p) return -(int64_t) EFAULT;
    jail_t *j = jail_find(jid);
    if (!j || j->state != JAIL_ACTIVE) return -(int64_t) ESRCH;
    if (!jail_is_descendant(p->jail_id, jid)) return -(int64_t) EPERM; // no escape upward
    if (!jail_can_fork(jid)) return -(int64_t) EAGAIN;
    jail_enter(p, jid);
    return 0;
}

int64_t sys_jail_get(uint32_t jid, kjail_info_t *uout) {
    proc_t *p = cur();
    if (!uout || !uptr_ok_w(uout, sizeof(kjail_info_t))) return -(int64_t) EFAULT;
    jail_t *j = jail_find(jid);
    if (!j) return -(int64_t) ESRCH;
    if (p && !jail_is_descendant(p->jail_id, jid)) return -(int64_t) EPERM;
    kjail_info_t info;
    memset(&info, 0, sizeof(info));
    info.id = j->id;
    info.parent_id = j->parent_id;
    info.flags = j->flags;
    info.nprocs = j->nprocs;
    info.max_procs = j->max_procs;
    info.creator_uid = j->creator_uid;
    memcpy(info.name, j->name, JAIL_NAME_MAX);
    memcpy(info.root, j->root, JAIL_ROOT_MAX);
    memcpy(uout, &info, sizeof(info));
    return 0;
}

int64_t sys_jail_list(uint32_t *uids, int max) {
    proc_t *p = cur();
    if (max < 0) return -(int64_t) EINVAL;
    if (max > 0 && (!uids || !uptr_ok_w(uids, (uint64_t) max * sizeof(uint32_t))))
        return -(int64_t) EFAULT;
    int n = 0;
    for (int i = 0; i < JAIL_MAX; i++) {
        if (g_jails[i].state == JAIL_UNUSED) continue;
        if (p && !jail_is_descendant(p->jail_id, g_jails[i].id)) continue;
        if (n < max) uids[n] = g_jails[i].id;
        n++;
    }
    return n;
}

int64_t sys_jail_remove(uint32_t jid) { return jail_remove(jid, cur()); }

int64_t sys_jail_self(void) {
    proc_t *p = cur();
    return p ? (int64_t) p->jail_id : 0;
}

int64_t sys_jail_set_auto(int on) {
    if (!host_priv()) return -(int64_t) EPERM;
    g_jail_auto_isolate = on ? 1 : 0;
    return 0;
}
