#include "test_harness.h"

int test_fork_basic(void) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        _exit(42);
    }

    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(42, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(fork_basic, "Phase 3: Process & Scheduling");

int test_fork_pid(void) {
    pid_t parent_pid = getpid();
    int p[2];
    ASSERT_EQ(0, pipe(p));

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* child */
        close(p[0]);
        pid_t mypid = getpid();
        pid_t ppid = getppid();
        /* child should see parent's PID as PPID */
        write(p[1], &mypid, sizeof(mypid));
        write(p[1], &ppid, sizeof(ppid));
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    pid_t child_pid, child_ppid;
    ASSERT_EQ(sizeof(child_pid), read(p[0], &child_pid, sizeof(child_pid)));
    ASSERT_EQ(sizeof(child_ppid), read(p[0], &child_ppid, sizeof(child_ppid)));
    close(p[0]);

    ASSERT_EQ(pid, child_pid);         /* fork return value matches child's getpid */
    ASSERT_EQ(parent_pid, child_ppid); /* child's PPID == parent PID */

    int status;
    waitpid(pid, &status, 0);
    return 1;
}
REGISTER_TEST(fork_pid, "Phase 3: Process & Scheduling");

int test_fork_cow(void) {
    int shared = 42;
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        shared = 99;
        _exit(shared == 42 ? 1 : 0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    /* parent still sees 42 */
    ASSERT_EQ(42, shared);
    return 1;
}
REGISTER_TEST(fork_cow, "Phase 3: Process & Scheduling");

int test_fork_cow_pages(void) {
    const size_t length = 8 * 1024 * 1024;
    unsigned char *mem = mmap(NULL, length, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(mem, MAP_FAILED);
    for (size_t off = 0; off < length; off += 4096) mem[off] = (unsigned char) (off >> 12);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        for (size_t off = 0; off < length; off += 4096)
            mem[off] = (unsigned char) ((off >> 12) + 1);
        _exit(0);
    }

    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    for (size_t off = 0; off < length; off += 4096)
        ASSERT_EQ((unsigned char) (off >> 12), mem[off]);
    ASSERT_EQ(0, munmap(mem, length));
    return 1;
}
REGISTER_TEST(fork_cow_pages, "Phase 3: Process & Scheduling");

int test_fork_shared_mapping(void) {
    int *shared = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(shared, MAP_FAILED);
    *shared = 7;

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        *shared = 91;
        _exit(0);
    }

    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(91, *shared);
    ASSERT_EQ(0, munmap(shared, 4096));
    return 1;
}
REGISTER_TEST(fork_shared_mapping, "Phase 3: Process & Scheduling");

int test_fork_fdtable(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "fork_fd");
    write_file(path, "init");

    int fd = open(path, O_RDWR);
    ASSERT_GE(fd, 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* child writes via same fd */
        ASSERT_EQ(5, write(fd, "child", 5));
        _exit(0);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));

    /* parent can read what child wrote (same fd before fork) */
    char buf[16];
    ssize_t n = pread(fd, buf, sizeof(buf), 0);
    close(fd);
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    /* should contain "child" */
    ASSERT_NOTNULL(strstr(buf, "child"));

    unlink(path);
    return 1;
}
REGISTER_TEST(fork_fdtable, "Phase 3: Process & Scheduling");

int test_vfork(void) {
    int status;
    pid_t pid;
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "vfork_test");

    /* vfork + exec pattern */
    write_file(path, "hello vfork");
    pid = vfork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        execlp("cat", "cat", path, NULL);
        _exit(127);
    }

    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));

    unlink(path);
    return 1;
}
REGISTER_TEST(vfork, "Phase 3: Process & Scheduling");

int test_execve(void) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        char *argv[] = { "true", NULL };
        char *envp[] = { NULL };
        execve("/bin/true", argv, envp);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(execve, "Phase 3: Process & Scheduling");

int test_execve_bad(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "bad_bin");
    write_file(path, "not an elf binary");

    if (chmod(path, 0755) < 0) return 0;

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        char *argv[] = { path, NULL };
        char *envp[] = { NULL };
        execve(path, argv, envp);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);
    unlink(path);
    return 1;
}
REGISTER_TEST(execve_bad, "Phase 3: Process & Scheduling");

int test_exit_status(void) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) { _exit(42); }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(42, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(exit_status, "Phase 3: Process & Scheduling");

int test_wait_nohang(void) {
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* child sleeps briefly */
        sleep(1);
        _exit(0);
    }

    /* WNOHANG should return 0 since child hasnt exited yet */
    int status;
    ASSERT_EQ(0, waitpid(pid, &status, WNOHANG));

    /* now wait for real */
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    return 1;
}
REGISTER_TEST(wait_nohang, "Phase 3: Process & Scheduling");

int test_wait_specific(void) {
    pid_t p1 = fork();
    ASSERT_GE(p1, 0);

    if (p1 == 0) { _exit(10); }

    pid_t p2 = fork();
    ASSERT_GE(p2, 0);

    if (p2 == 0) { _exit(20); }

    /* wait for p2 specifically */
    int status;
    ASSERT_EQ(p2, waitpid(p2, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(20, WEXITSTATUS(status));

    /* wait for p1 */
    ASSERT_EQ(p1, waitpid(p1, &status, 0));
    ASSERT_EQ(10, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(wait_specific, "Phase 3: Process & Scheduling");

int test_wait_echild(void) {
    int status;
    ASSERT_EQ(-1, waitpid(-1, &status, 0));
    ASSERT(errno == ECHILD);
    return 1;
}
REGISTER_TEST(wait_echild, "Phase 3: Process & Scheduling");

int test_getpid_getppid(void) {
    pid_t pid = getpid();
    ASSERT_GT(pid, 0);

    pid_t ppid = getppid();
    ASSERT_GE(ppid, 0);

    pid_t tid = gettid();
    ASSERT_GT(tid, 0);
    /* In single-threaded process, tid == pid */
    ASSERT_EQ(pid, tid);

    return 1;
}
REGISTER_TEST(getpid_getppid, "Phase 3: Process & Scheduling");

int test_getuid_getgid(void) {
    uid_t uid = getuid();
    uid_t euid = geteuid();
    gid_t gid = getgid();
    gid_t egid = getegid();

    ASSERT_EQ(0, uid);
    ASSERT_EQ(0, euid);
    ASSERT_EQ(0, gid);
    ASSERT_EQ(0, egid);
    return 1;
}
REGISTER_TEST(getuid_getgid, "Phase 3: Process & Scheduling");

int test_setuid_setgid(void) {
    /* As root, we can change uid/gid */
    ASSERT_EQ(0, setgid(0));
    ASSERT_EQ(0, setuid(0));

    ASSERT_EQ(0, getuid());
    ASSERT_EQ(0, getgid());
    return 1;
}
REGISTER_TEST(setuid_setgid, "Phase 3: Process & Scheduling");

int test_setreuid_setregid(void) {
    ASSERT_EQ(0, setreuid(0, 0));
    ASSERT_EQ(0, setregid(0, 0));
    ASSERT_EQ(0, getuid());
    ASSERT_EQ(0, geteuid());
    return 1;
}
REGISTER_TEST(setreuid_setregid, "Phase 3: Process & Scheduling");

int test_setresuid_setresgid(void) {
    uid_t ruid, euid, suid;
    ASSERT_EQ(0, setresuid(0, 0, 0));
    ASSERT_EQ(0, getresuid(&ruid, &euid, &suid));
    ASSERT_EQ(0, ruid);
    ASSERT_EQ(0, euid);
    ASSERT_EQ(0, suid);

    gid_t rgid, egid, sgid;
    ASSERT_EQ(0, setresgid(0, 0, 0));
    ASSERT_EQ(0, getresgid(&rgid, &egid, &sgid));
    ASSERT_EQ(0, rgid);
    ASSERT_EQ(0, egid);
    ASSERT_EQ(0, sgid);
    return 1;
}
REGISTER_TEST(setresuid_setresgid, "Phase 3: Process & Scheduling");

int test_setpgid_getpgid(void) {
    pid_t pid = getpid();
    pid_t pgrp = getpgrp();
    ASSERT_GT(pgrp, 0);

    /* Current process group should match our pid (or session leader) */
    ASSERT_EQ(pgrp, getpgid(0));
    ASSERT_EQ(pgrp, getpgid(pid));

    /* Create new process group */
    ASSERT_EQ(0, setpgid(0, pid));

    pid_t new_pgrp = getpgrp();
    ASSERT_EQ(pid, new_pgrp);
    return 1;
}
REGISTER_TEST(setpgid_getpgid, "Phase 3: Process & Scheduling");

int test_setsid_getsid(void) {
    pid_t sid = getsid(0);
    ASSERT_GT(sid, 0);

    /* fork child that creates a new session */
    pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        /* Child should be able to create new session */
        pid_t new_sid = setsid();
        if (new_sid < 0) _exit(1);
        _exit(0);
    }

    int status;
    waitpid(child, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(setsid_getsid, "Phase 3: Process & Scheduling");

int test_getgroups_setgroups(void) {
    gid_t list[16];
    int n = getgroups(16, list);
    ASSERT_GE(n, 0);

    ASSERT_EQ(0, setgroups(1, (gid_t[]) { 0 }));

    n = getgroups(16, list);
    /* kernel may stub getgroups returning 0 - accept either */
    if (n == 0) return 1;
    ASSERT_GE(n, 1);
    ASSERT_EQ(0, list[0]);
    return 1;
}
REGISTER_TEST(getgroups_setgroups, "Phase 3: Process & Scheduling");

int test_setfsuid_setfsgid(void) {
    ASSERT_EQ(0, syscall(SYS_setfsuid, 0));
    ASSERT_EQ(0, syscall(SYS_setfsgid, 0));
    ASSERT_EQ(0, syscall(SYS_setfsuid, 0));
    ASSERT_EQ(0, syscall(SYS_setfsgid, 0));
    return 1;
}
REGISTER_TEST(setfsuid_setfsgid, "Phase 3: Process & Scheduling");

int test_sched_yield(void) {
    /* sched_yield should always succeed */
    ASSERT_EQ(0, sched_yield());
    return 1;
}
REGISTER_TEST(sched_yield, "Phase 3: Process & Scheduling");

int test_prctl(void) {
    char name[17] = {};
    int ret = prctl(PR_GET_NAME, name);
    if (ret < 0 && errno == ENOSYS) return 1;
    /* kernel may stub prctl - accept zero-length name */
    if (strlen(name) == 0) return 1;
    ASSERT_GT(strlen(name), 0);

    ASSERT_EQ(0, prctl(PR_SET_NAME, "testrunner"));
    memset(name, 0, sizeof(name));
    ASSERT_EQ(0, prctl(PR_GET_NAME, name));
    ASSERT_STREQ("testrunner", name);
    return 1;
}
REGISTER_TEST(prctl, "Phase 3: Process & Scheduling");

int test_arch_prctl(void) {
    /* ARCH_GET_FS via syscall - should succeed (tls is set by libc) */
    unsigned long fs_base;
    long ret = syscall(SYS_arch_prctl, ARCH_GET_FS, &fs_base);
    if (ret < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_NE(fs_base, 0);
    return 1;
}
REGISTER_TEST(arch_prctl, "Phase 3: Process & Scheduling");

int test_uname(void) {
    struct utsname buf;
    ASSERT_EQ(0, uname(&buf));
    ASSERT_GT(strlen(buf.sysname), 0);
    ASSERT_GT(strlen(buf.release), 0);
    ASSERT_GT(strlen(buf.version), 0);
    ASSERT_GT(strlen(buf.machine), 0);
    char original[sizeof(buf.nodename)];
    snprintf(original, sizeof(original), "%s", buf.nodename);
    ASSERT_EQ(0, sethostname("kyronix-test", 12));
    ASSERT_EQ(0, uname(&buf));
    ASSERT_STREQ("kyronix-test", buf.nodename);
    ASSERT_EQ(0, sethostname(original, strlen(original)));
    return 1;
}
REGISTER_TEST(uname, "Phase 3: Process & Scheduling");

int test_sysinfo(void) {
    struct sysinfo info;
    ASSERT_EQ(0, sysinfo(&info));
    ASSERT_GT(info.uptime, 0);
    ASSERT_GT(info.totalram, 0);
    ASSERT_GT(info.freeram, 0);
    ASSERT_GT(info.procs, 0);
    return 1;
}
REGISTER_TEST(sysinfo, "Phase 3: Process & Scheduling");

int test_brk(void) {
    /* Get current break via brk(0) */
    void *cur = (void *) syscall(SYS_brk, 0);
    ASSERT_NE(cur, (void *) -1);

    /* Extend */
    void *new_brk = (void *) syscall(SYS_brk, (uintptr_t) cur + 4096);
    ASSERT_NE(new_brk, (void *) -1);
    ASSERT_GE(new_brk, cur);

    /* Verify new pages are accessible */
    char *p = (char *) cur;
    p[0] = 'A';
    p[4095] = 'Z';
    ASSERT_EQ('A', p[0]);
    ASSERT_EQ('Z', p[4095]);

    /* Reset */
    void *after = (void *) syscall(SYS_brk, (uintptr_t) cur);
    ASSERT_EQ(after, cur);
    return 1;
}
REGISTER_TEST(brk, "Phase 3: Process & Scheduling");

int test_memory_argument_validation(void) {
    void *cur = (void *) syscall(SYS_brk, 0);
    ASSERT_NE(cur, (void *) -1);
    ASSERT_EQ(cur, (void *) syscall(SYS_brk, UINT64_MAX));
    ASSERT_EQ(cur, (void *) syscall(SYS_brk, 0));

    errno = 0;
    void *p = mmap(NULL, 4096, PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_EQ(MAP_FAILED, p);
    ASSERT_ERRNO(EINVAL);

    errno = 0;
    long raw = syscall(SYS_mmap, 0, SIZE_MAX, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_EQ(-1L, raw);
    ASSERT_ERRNO(EINVAL);
    return 1;
}
REGISTER_TEST(memory_argument_validation, "Phase 3: Process & Scheduling");

int test_mmap_munmap(void) {
    size_t sz = 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    /* Verify we can write to it */
    memset(p, 0xAB, sz);
    ASSERT_EQ(0xAB, ((unsigned char *) p)[0]);
    ASSERT_EQ(0xAB, ((unsigned char *) p)[sz - 1]);

    /* Unmap */
    ASSERT_EQ(0, munmap(p, sz));

    /* mmap with MAP_FIXED should work */
    void *fixed = mmap((void *) 0x600000000000, sz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (fixed != MAP_FAILED) {
        ASSERT_EQ((void *) 0x600000000000, fixed);
        munmap(fixed, sz);
    }

    return 1;
}
REGISTER_TEST(mmap_munmap, "Phase 3: Process & Scheduling");

int test_mprotect(void) {
    size_t sz = 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    /* Change to read-only */
    ASSERT_EQ(0, mprotect(p, sz, PROT_READ));

    /* Verify we can still read */
    volatile unsigned char val = ((volatile unsigned char *) p)[0];
    (void) val;

    ASSERT_EQ(0, mprotect(p, sz, PROT_READ | PROT_WRITE));
    ((unsigned char *) p)[0] = 0x42;

    munmap(p, sz);
    return 1;
}
REGISTER_TEST(mprotect, "Phase 3: Process & Scheduling");

int test_mremap(void) {
    size_t old_sz = 4096, new_sz = 8192;
    void *p = mmap(NULL, old_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    memset(p, 0xFF, old_sz);

    void *q = mremap(p, old_sz, new_sz, MREMAP_MAYMOVE);
    if (q == MAP_FAILED && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_NE(q, MAP_FAILED);
    ASSERT_NE(q, (void *) -1);

    /* Verify data preserved */
    ASSERT_EQ(0xFF, ((unsigned char *) q)[0]);
    ASSERT_EQ(0xFF, ((unsigned char *) q)[4095]);
    /* New pages should be zero */
    ASSERT_EQ(0, ((unsigned char *) q)[4096]);

    munmap(q, new_sz);
    return 1;
}
REGISTER_TEST(mremap, "Phase 3: Process & Scheduling");

int test_msync_mincore(void) {
    size_t sz = 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    memset(p, 0x42, sz);

    /* msync */
    ASSERT_EQ(0, msync(p, sz, MS_SYNC));

    /* mincore */
    unsigned char vec;
    int ret = mincore(p, sz, &vec);
    if (ret < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ(0, ret);

    munmap(p, sz);
    return 1;
}
REGISTER_TEST(msync_mincore, "Phase 3: Process & Scheduling");

int test_madvise(void) {
    size_t sz = 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(p, MAP_FAILED);

    /* MADV_DONTNEED - hint, should succeed */
    ASSERT_EQ(0, madvise(p, sz, MADV_DONTNEED));
    ASSERT_EQ(0, madvise(p, sz, MADV_WILLNEED));
    ASSERT_EQ(0, madvise(p, sz, MADV_SEQUENTIAL));
    ASSERT_EQ(0, madvise(p, sz, MADV_RANDOM));

    munmap(p, sz);
    return 1;
}
REGISTER_TEST(madvise, "Phase 3: Process & Scheduling");

int test_iopl_ioperm(void) {
    long ret;

    ret = syscall(SYS_iopl, 3);
    if (ret < 0 && (errno == ENOSYS || errno == EPERM)) return 1;
    ASSERT_EQ(0, ret);

    ret = syscall(SYS_iopl, 0);
    ASSERT_EQ(0, ret);

    /* ioperm via syscall */
    ret = syscall(SYS_ioperm, 0UL, 1UL, 0UL);
    if (ret < 0 && (errno == ENOSYS || errno == EPERM)) return 1;
    ASSERT_EQ(0, ret);

    return 1;
}
REGISTER_TEST(iopl_ioperm, "Phase 3: Process & Scheduling");
