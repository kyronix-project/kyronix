#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NS_TYPE_USER 1
#define NS_TYPE_PID 2
#define NS_TYPE_MNT 3
#define NS_TYPE_UTS 4
#define NS_TYPE_IPC 5
#define NS_TYPE_NET 6
#define NS_TYPE_CGROUP 7

#define NS_PID_MAX 64
#define NS_UID_GID_MAP_MAX 5

#define CLONE_NEWNS 0x00020000
#define CLONE_NEWCGROUP 0x02000000
#define CLONE_NEWUTS 0x04000000
#define CLONE_NEWIPC 0x08000000
#define CLONE_NEWUSER 0x10000000
#define CLONE_NEWPID 0x20000000
#define CLONE_NEWNET 0x40000000
#define NS_CLONE_ALL                                                                              \
    (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | \
     CLONE_NEWNET)

typedef struct ns_common {
    uint32_t type;
    uint32_t refcnt;
    uint8_t dynamic;
} ns_common_t;

typedef struct ns_uid_gid_map {
    uint32_t inside;
    uint32_t outside;
    uint32_t count;
} ns_uid_gid_map_t;

typedef struct user_namespace {
    ns_common_t common;
    struct user_namespace *parent;
    uint32_t owner;
    uint8_t map_written;
    uint8_t gid_map_written;
    uint8_t groups_disabled;
    uint8_t nuid_map;
    uint8_t ngid_map;
    ns_uid_gid_map_t uid_map[NS_UID_GID_MAP_MAX];
    ns_uid_gid_map_t gid_map[NS_UID_GID_MAP_MAX];
} user_namespace_t;

typedef struct pid_namespace {
    ns_common_t common;
    struct pid_namespace *parent;
    int level;
    uint32_t next_pid;
    uint32_t local_of_global[NS_PID_MAX];
    uint32_t global_of_local[NS_PID_MAX];
} pid_namespace_t;

typedef struct uts_namespace {
    ns_common_t common;
    char hostname[65];
} uts_namespace_t;

typedef struct simple_namespace {
    ns_common_t common;
} simple_namespace_t;

#define NS_MNT_MAX 64

typedef struct mnt_mount {
    char target[256];
    char source[128];
    char fstype[16];
    struct vfs_node *root;
    uint32_t flags;
    uint8_t used;
} mnt_mount_t;

typedef struct mnt_namespace {
    ns_common_t common;
    struct vfs_node *root;
    uint32_t root_flags;
    uint32_t nmounts;
    mnt_mount_t mounts[NS_MNT_MAX];
} mnt_namespace_t;

typedef struct nsproxy {
    uint32_t refcnt;
    uint8_t dynamic;
    user_namespace_t *user_ns;
    pid_namespace_t *pid_ns;
    pid_namespace_t *pid_ns_for_children;
    mnt_namespace_t *mnt_ns;
    uts_namespace_t *uts_ns;
    simple_namespace_t *ipc_ns;
    simple_namespace_t *net_ns;
    simple_namespace_t *cgroup_ns;
} nsproxy_t;

struct proc;
struct vfs_node;

extern nsproxy_t init_nsproxy;
extern user_namespace_t init_user_ns;
extern pid_namespace_t init_pid_ns;
extern mnt_namespace_t init_mnt_ns;
extern uts_namespace_t init_uts_ns;
extern simple_namespace_t init_ipc_ns;
extern simple_namespace_t init_net_ns;
extern simple_namespace_t init_cgroup_ns;

void ns_init(void);
nsproxy_t *nsproxy_get(nsproxy_t *ns);
void nsproxy_put(nsproxy_t *ns);
nsproxy_t *nsproxy_clone(const nsproxy_t *src);
int ns_child_prepare(struct proc *parent, struct proc *child, uint64_t clone_flags, bool thread);
int ns_unshare(struct proc *p, uint64_t flags);
int ns_setns(struct proc *p, struct vfs_node *node, int nstype);
void ns_proc_register(struct proc *p);
void ns_proc_unregister(struct proc *p);

mnt_namespace_t *ns_mnt_current(void);
mnt_namespace_t *ns_mnt_clone(mnt_namespace_t *src);
void ns_mnt_set(struct proc *p, mnt_namespace_t *nm);
void ns_mnt_put(mnt_namespace_t *nm);
mnt_mount_t *ns_mnt_find(mnt_namespace_t *ns, const char *target);

uint32_t ns_proc_pid(const struct proc *p);
uint32_t ns_proc_pid_in(const struct proc *p, const pid_namespace_t *ns);
uint32_t ns_pid_to_global(const pid_namespace_t *ns, uint32_t local);
int ns_pid_chain(const struct proc *p, uint32_t *out, int max);
const char *ns_type_name(uint32_t type);
ns_common_t *ns_for_proc(const struct proc *p, uint32_t type, bool for_children);

int ns_uid_to_local(const user_namespace_t *ns, uint32_t kuid, uint32_t *out);
int ns_gid_to_local(const user_namespace_t *ns, uint32_t kgid, uint32_t *out);
int ns_local_to_kuid(const user_namespace_t *ns, uint32_t uid, uint32_t *out);
int ns_local_to_kgid(const user_namespace_t *ns, uint32_t gid, uint32_t *out);
bool ns_user_is_initial(const struct proc *p);
bool ns_user_is_root(const struct proc *p);
const char *ns_uts_hostname(const struct proc *p);
int ns_uts_sethostname(struct proc *p, const char *name, uint64_t len);
int ns_user_map_read(struct proc *target, bool gid, char *buf, uint64_t len);
int ns_user_map_write(struct proc *target, bool gid, const char *buf, uint64_t len);
int ns_user_setgroups_read(struct proc *target, char *buf, uint64_t len);
int ns_user_setgroups_write(struct proc *target, const char *buf, uint64_t len);
bool ns_mnt_resolve(mnt_namespace_t *ns, const char *path, struct vfs_node **node,
                    const char **suffix, uint32_t *flags);
uint32_t ns_mnt_path_flags(const char *path);
bool ns_mnt_path_readonly(const char *path);
int ns_mnt_list(char *buf, uint64_t len, bool info);

int64_t sys_unshare(uint64_t flags);
int64_t sys_setns(int fd, int nstype);
