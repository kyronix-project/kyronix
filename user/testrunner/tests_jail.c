#include "test_harness.h"

#define SYS_jail_create 500
#define SYS_jail_attach 501
#define SYS_jail_get 502
#define SYS_jail_list 503
#define SYS_jail_remove 504
#define SYS_jail_self 505
#define SYS_jail_set_auto 506

#define JAILF_FS 0x01u
#define JAILF_PID 0x02u
#define JAILF_IPC 0x04u
#define JAILF_PRIV 0x08u
#define JAILF_ALL (JAILF_FS | JAILF_PID | JAILF_IPC | JAILF_PRIV)

typedef struct {
    char root[256];
    char name[32];
    unsigned flags;
    unsigned max_procs;
    unsigned attach;
} kjail_conf_t;

typedef struct {
    unsigned id, parent_id, flags, nprocs, max_procs, creator_uid;
    char name[32];
    char root[256];
} kjail_info_t;

static long jc(kjail_conf_t *c) { return syscall(SYS_jail_create, c); }
static long ja(unsigned id) { return syscall(SYS_jail_attach, id); }
static long jself(void) { return syscall(SYS_jail_self); }
static long jget(unsigned id, kjail_info_t *o) { return syscall(SYS_jail_get, id, o); }
static long jrm(unsigned id) { return syscall(SYS_jail_remove, id); }

static long mkjail(const char *root, unsigned flags, unsigned max_procs) {
    kjail_conf_t c;
    memset(&c, 0, sizeof(c));
    strncpy(c.root, root, sizeof(c.root) - 1);
    c.flags = flags;
    c.max_procs = max_procs;
    c.attach = 0;
    return jc(&c);
}

int test_jail_self_host(void) {
    ASSERT_EQ(0, jself());
    return 1;
}
REGISTER_TEST(jail_self_host, "Phase 9: Jails");

int test_jail_create_get(void) {
    long id = mkjail("/tmp/jc", JAILF_ALL, 8);
    ASSERT_GT(id, 0);
    ASSERT_EQ(0, jself()); /* create does not attach the caller */
    kjail_info_t info;
    ASSERT_EQ(0, jget((unsigned) id, &info));
    ASSERT_EQ((unsigned) id, info.id);
    ASSERT_EQ((unsigned) JAILF_ALL, info.flags);
    ASSERT_EQ(8u, info.max_procs);
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_create_get, "Phase 9: Jails");

int test_jail_fs_confine(void) {
    ASSERT_EQ(0, mkdir_p("/tmp/jfs"));
    ASSERT_TRUE(write_file("/tmp/jfs/probe", "hi"));
    ASSERT_TRUE(write_file("/tmp/jsecret", "s")); /* host-only, outside jail root */

    long id = mkjail("/tmp/jfs", JAILF_FS, 0);
    ASSERT_GT(id, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (ja((unsigned) id) != 0) _exit(11);
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) _exit(12);
        if (strcmp(cwd, "/") != 0) _exit(13);
        int f = open("/probe", O_RDONLY); /* jail-relative -> /tmp/jfs/probe */
        if (f < 0) _exit(14);
        close(f);
        if (open("/tmp/jfs/probe", O_RDONLY) >= 0) _exit(15); /* re-rooted, must miss */
        if (open("/../jsecret", O_RDONLY) >= 0) _exit(16);    /* escape clamped to root */
        if (open("/../../../jsecret", O_RDONLY) >= 0) _exit(17);
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_fs_confine, "Phase 9: Jails");

int test_jail_pid_isolation(void) {
    long id = mkjail("/", JAILF_PID, 0);
    ASSERT_GT(id, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        pid_t host_parent = getppid();
        if (ja((unsigned) id) != 0) _exit(11);
        errno = 0;
        if (kill(1, 0) == 0) _exit(12); /* init invisible from inside the jail */
        if (errno != ESRCH) _exit(13);
        errno = 0;
        if (kill(host_parent, 0) == 0) _exit(14); /* host parent invisible too */
        if (errno != ESRCH) _exit(15);
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_pid_isolation, "Phase 9: Jails");

int test_jail_priv_confine(void) {
    long id = mkjail("/", JAILF_PRIV, 0);
    ASSERT_GT(id, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (ja((unsigned) id) != 0) _exit(11);
        errno = 0;
        if (sethostname("jailtest", 8) == 0) _exit(12); /* host-global op denied in jail */
        if (errno != EPERM) _exit(13);
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_priv_confine, "Phase 9: Jails");

int test_jail_create_unpriv(void) {
    long id = mkjail("/", JAILF_PRIV, 0);
    ASSERT_GT(id, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (ja((unsigned) id) != 0) _exit(11);
        kjail_conf_t c;
        memset(&c, 0, sizeof(c));
        strncpy(c.root, "/", sizeof(c.root) - 1);
        c.flags = JAILF_FS;
        errno = 0;
        if (jc(&c) >= 0) _exit(12); /* jail creation must be denied without privilege */
        if (errno != EPERM) _exit(13);
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_create_unpriv, "Phase 9: Jails");

int test_jail_epoll(void) {
    ASSERT_EQ(0, mkdir_p("/tmp/jep"));
    long id = mkjail("/tmp/jep", JAILF_FS, 0); /* root has no /dev/null */
    ASSERT_GT(id, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (ja((unsigned) id) != 0) _exit(11);
        int ep = epoll_create1(0); /* internal /dev/null handle must dodge the jail root */
        if (ep < 0) _exit(12);
        close(ep);
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) id);
    return 1;
}
REGISTER_TEST(jail_epoll, "Phase 9: Jails");

int test_jail_no_escape(void) {
    long a = mkjail("/", JAILF_ALL, 0);
    long b = mkjail("/", JAILF_ALL, 0);
    ASSERT_GT(a, 0);
    ASSERT_GT(b, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        if (ja((unsigned) a) != 0) _exit(11);
        errno = 0;
        if (ja((unsigned) b) == 0) _exit(12); /* b is a sibling, not a descendant of a */
        if (errno != EPERM) _exit(13);
        errno = 0;
        if (ja(0) == 0) _exit(14); /* cannot return to host */
        _exit(0);
    }
    int st;
    ASSERT_EQ(pid, waitpid(pid, &st, 0));
    ASSERT_TRUE(WIFEXITED(st));
    ASSERT_EQ(0, WEXITSTATUS(st));
    jrm((unsigned) a);
    jrm((unsigned) b);
    return 1;
}
REGISTER_TEST(jail_no_escape, "Phase 9: Jails");
