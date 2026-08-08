#include "jail.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "proc/proc.h"

#define EPERM 1
#define ENOENT 2
#define EINVAL 22
#define ENOSPC 28

jail_t g_jails[JAIL_MAX];
uint8_t g_jail_auto_isolate;

void jail_init(void) {
    memset(g_jails, 0, sizeof(g_jails));
    g_jail_auto_isolate = 0;
    log_info("JAIL: subsystem ready (%d slots)", JAIL_MAX);
}

jail_t *jail_find(uint32_t id) {
    if (id == JAIL_HOST) return NULL;
    for (int i = 0; i < JAIL_MAX; i++)
        if (g_jails[i].state != JAIL_UNUSED && g_jails[i].id == id) return &g_jails[i];
    return NULL;
}

void path_canon(const char *in, char *out, size_t sz) {
    size_t starts[128];
    int n = 0;
    size_t w = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - s);
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            if (n > 0) {
                n--;
                w = starts[n];
            }
            continue;
        }
        if (n >= 128 || w + 1 + len >= sz) break;
        starts[n++] = w;
        out[w++] = '/';
        memcpy(out + w, s, len);
        w += len;
    }
    if (w == 0) out[w++] = '/';
    out[w] = '\0';
}

void jail_canon_clamp(char *path, size_t sz, const char *root) {
    size_t rlen = strlen(root);
    if (rlen == 0) return; /* host: keep current behavior */
    bool under_root = strncmp(path, root, rlen) == 0 &&
                      (path[rlen] == '\0' || path[rlen] == '/');
    const char *sub = under_root ? path + rlen : path;
    char canon[512];
    path_canon(sub, canon, sizeof(canon));
    char tmp[768];
    if (canon[0] == '/' && canon[1] == '\0')
        snprintf(tmp, sizeof(tmp), "%s", root);
    else
        snprintf(tmp, sizeof(tmp), "%s%s", root, canon);
    strncpy(path, tmp, sz - 1);
    path[sz - 1] = '\0';
}

const char *jail_root_current(void) {
    proc_t *p = g_current_proc;
    if (!p) return "";
    jail_t *j = jail_find(p->jail_id);
    if (j && (j->flags & JAILF_FS) && j->root[0]) return j->root;
    return "";
}

void jail_strip_root(char *abs, size_t sz) {
    (void) sz;
    const char *root = jail_root_current();
    size_t rlen = strlen(root);
    if (rlen == 0 || strncmp(abs, root, rlen) != 0) return;
    const char *sub = abs + rlen;
    if (sub[0] == '\0') {
        abs[0] = '/';
        abs[1] = '\0';
        return;
    }
    memmove(abs, sub, strlen(sub) + 1);
}

bool jail_is_descendant(uint32_t a, uint32_t b) {
    if (a == b) return true;
    if (a == JAIL_HOST) return true;
    uint32_t cur = b;
    for (int guard = 0; cur != JAIL_HOST && guard <= JAIL_MAX; guard++) {
        jail_t *j = jail_find(cur);
        if (!j) break;
        if (j->parent_id == a) return true;
        cur = j->parent_id;
    }
    return false;
}

bool jail_can_see(const struct proc *observer, const struct proc *target) {
    if (!observer) return true; /* kernel context */
    jail_t *oj = jail_find(observer->jail_id);
    if (!oj || !(oj->flags & JAILF_PID)) return true;
    return observer->jail_id == target->jail_id;
}

bool jail_host_priv(const struct proc *p) {
    if (!p) return true;
    if (p->euid != 0) return false;
    jail_t *j = jail_find(p->jail_id);
    if (j && (j->flags & JAILF_PRIV)) return false;
    return true;
}

void jail_ref(uint32_t jid) {
    jail_t *j = jail_find(jid);
    if (j) j->nprocs++;
}

void jail_unref(uint32_t jid) {
    jail_t *j = jail_find(jid);
    if (!j) return;
    if (j->nprocs) j->nprocs--;
}

void jail_retire(uint32_t jid) {
    jail_t *j = jail_find(jid);
    if (!j) return;
    j->state = JAIL_DYING;
}

void jail_reap(uint32_t jid) {
    jail_t *j = jail_find(jid);
    if (j && j->state == JAIL_DYING && j->nprocs == 0) j->state = JAIL_UNUSED;
}

bool jail_can_fork(uint32_t jid) {
    jail_t *j = jail_find(jid);
    if (!j) return true; /* host */
    if (j->state != JAIL_ACTIVE) return false;
    if (j->max_procs && j->nprocs >= j->max_procs) return false;
    return true;
}

void jail_enter(struct proc *p, uint32_t jid) {
    if (!p || p->jail_id == jid) return;
    jail_unref(p->jail_id);
    p->jail_id = jid;
    p->jail_exempt = 0;
    jail_ref(jid);
    jail_t *j = jail_find(jid);
    if (j && (j->flags & JAILF_FS)) {
        const char *r = j->root[0] ? j->root : "/";
        strncpy(p->cwd, r, sizeof(p->cwd) - 1);
        p->cwd[sizeof(p->cwd) - 1] = '\0';
    }
}

int jail_create(uint32_t parent_jid, const kjail_conf_t *cfg, uint32_t creator_uid) {
    if (!cfg) return -EINVAL;

    const char *proot = "";
    if (parent_jid != JAIL_HOST) {
        jail_t *pj = jail_find(parent_jid);
        if (!pj) return -EINVAL;
        proot = pj->root;
    }

    char combined[512];
    snprintf(combined, sizeof(combined), "%s/%s", proot, cfg->root);
    char canon[512];
    path_canon(combined, canon, sizeof(canon));

    size_t plen = strlen(proot);
    if (plen && strncmp(canon, proot, plen) != 0) {
        strncpy(canon, proot, sizeof(canon) - 1);
        canon[sizeof(canon) - 1] = '\0';
    }
    if (strlen(canon) >= JAIL_ROOT_MAX) return -EINVAL;

    for (int i = 0; i < JAIL_MAX; i++) {
        if (g_jails[i].state != JAIL_UNUSED) continue;
        jail_t *j = &g_jails[i];
        memset(j, 0, sizeof(*j));
        j->state = JAIL_ACTIVE;
        j->id = (uint32_t) (i + 1);
        j->parent_id = parent_jid;
        j->flags = cfg->flags & JAILF_ALL;
        j->max_procs = cfg->max_procs;
        j->creator_uid = creator_uid;
        strncpy(j->root, canon, JAIL_ROOT_MAX - 1);
        strncpy(j->name, cfg->name, JAIL_NAME_MAX - 1);
        log_info("JAIL: created id=%u parent=%u root=%s flags=0x%x", j->id, parent_jid, j->root,
                 j->flags);
        return (int) j->id;
    }
    return -ENOSPC;
}

int jail_remove(uint32_t jid, const struct proc *requester) {
    jail_t *j = jail_find(jid);
    if (!j) return -ENOENT;
    if (!jail_host_priv(requester) &&
        !(requester && jail_is_descendant(requester->jail_id, jid) && requester->jail_id != jid))
        return -EPERM;
    if (j->nprocs == 0)
        j->state = JAIL_UNUSED;
    else
        j->state = JAIL_DYING;
    return 0;
}
