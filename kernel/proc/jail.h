#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define JAIL_MAX 32
#define JAIL_NAME_MAX 32
#define JAIL_ROOT_MAX 256

#define JAIL_HOST 0u

#define JAILF_FS 0x01u
#define JAILF_PID 0x02u
#define JAILF_IPC 0x04u
#define JAILF_PRIV 0x08u
#define JAILF_ALL (JAILF_FS | JAILF_PID | JAILF_IPC | JAILF_PRIV)
#define JAILF_EXEMPT 0x80u

#define JAIL_UNUSED 0
#define JAIL_ACTIVE 1
#define JAIL_DYING 2

#define SYS_jail_create 500
#define SYS_jail_attach 501
#define SYS_jail_get 502
#define SYS_jail_list 503
#define SYS_jail_remove 504
#define SYS_jail_self 505
#define SYS_jail_set_auto 506

typedef struct jail {
    int state;
    uint32_t id;
    uint32_t parent_id;
    uint32_t flags;
    char name[JAIL_NAME_MAX];
    char root[JAIL_ROOT_MAX];
    uint32_t nprocs;
    uint32_t max_procs;
    uint32_t creator_uid;
} jail_t;

typedef struct {
    char root[JAIL_ROOT_MAX];
    char name[JAIL_NAME_MAX];
    uint32_t flags;
    uint32_t max_procs;
    uint32_t attach;
} kjail_conf_t;

typedef struct {
    uint32_t id, parent_id, flags, nprocs, max_procs, creator_uid;
    char name[JAIL_NAME_MAX];
    char root[JAIL_ROOT_MAX];
} kjail_info_t;

struct proc;

extern jail_t g_jails[JAIL_MAX];
extern uint8_t g_jail_auto_isolate;

void jail_init(void);
jail_t *jail_find(uint32_t id);

int jail_create(uint32_t parent_jid, const kjail_conf_t *cfg, uint32_t creator_uid);
void jail_ref(uint32_t jid);                   /* a new proc joined jid (fork inherit) */
void jail_unref(uint32_t jid);                 /* a live proc left jid */
void jail_retire(uint32_t jid);                /* kernel policy: destroy after last proc exits */
void jail_reap(uint32_t jid);                  /* release a dying jail after zombie reap */
bool jail_can_fork(uint32_t jid);              /* false if jid is dting or at max_procs */
void jail_enter(struct proc *p, uint32_t jid); /* transition current proc into jid */
int jail_remove(uint32_t jid, const struct proc *requester);

bool jail_is_descendant(uint32_t a, uint32_t b);

bool jail_can_see(const struct proc *observer, const struct proc *target);
bool jail_host_priv(const struct proc *p); /* euid==0 and not confined by JAILF_PRIV */

const char *jail_root_current(void);                   /* "" if host / no JAILF_FS */
void path_canon(const char *in, char *out, size_t sz); /* clamp ".." at "/" */
void jail_canon_clamp(char *path, size_t sz, const char *root);
void jail_strip_root(char *abs, size_t sz); /* host->jail-relative for getcwd */
