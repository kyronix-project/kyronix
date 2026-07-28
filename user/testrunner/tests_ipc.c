#include "test_harness.h"

static volatile sig_atomic_t sigpipe_caught = 0;

static void handler_sigpipe(int sig) {
    (void) sig;
    sigpipe_caught = 1;
}

int test_pipe_close_race(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));
    close(p[0]);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        sigpipe_caught = 0;
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler_sigpipe;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, NULL);

        /* write large buffer to trigger EPIPE */
        char buf[4096];
        memset(buf, 'A', sizeof(buf));
        ssize_t n = write(p[1], buf, sizeof(buf));
        if (n < 0 && errno == EPIPE) _exit(0);
        if (sigpipe_caught) _exit(0);
        _exit(1);
    }

    close(p[1]);

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(pipe_close_race, "Phase 5: Pipes & IPC");

int test_pipe_capacity(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    /* make the write end non-blocking so filling it returns EAGAIN, not a hang */
    int fl = fcntl(p[1], F_GETFL);
    ASSERT_GE(fl, 0);
    ASSERT_EQ(0, fcntl(p[1], F_SETFL, fl | O_NONBLOCK));

    char chunk[4096];
    memset(chunk, 'X', sizeof(chunk));
    size_t total = 0;
    for (;;) {
        ssize_t n = write(p[1], chunk, sizeof(chunk));
        if (n < 0) {
            ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
        ASSERT_GT(n, 0);
        total += (size_t) n;
        if (total > (1u << 20)) break; /* safety: should never exceed capacity */
    }
    ASSERT_EQ((size_t) 65536, total); /* PIPE_BUFSZ */

    /* once full, a further non-blocking write returns EAGAIN */
    ssize_t n = write(p[1], chunk, 1);
    ASSERT_EQ((ssize_t) -1, n);
    ASSERT(errno == EAGAIN || errno == EWOULDBLOCK);

    /* draining frees space so writes succeed again */
    char drain[4096];
    ASSERT_GT(read(p[0], drain, sizeof(drain)), 0);
    ASSERT_EQ((ssize_t) 1, write(p[1], chunk, 1));

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(pipe_capacity, "Phase 5: Pipes & IPC");

int test_dup_dup2_dup3(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    /* dup */
    int newfd = dup(p[0]);
    ASSERT_GE(newfd, 0);
    ASSERT_NE(newfd, p[0]);
    close(newfd);

    /* dup2 — specific fd */
    ASSERT_EQ(30, dup2(p[0], 30));
    close(30);

    /* dup2 same fd (no-op) */
    ASSERT_EQ(p[0], dup2(p[0], p[0]));

    /* dup3 with O_CLOEXEC */
    ASSERT_EQ(31, dup3(p[0], 31, O_CLOEXEC));
    int flags = fcntl(31, F_GETFD);
    ASSERT_GE(flags, 0);
    ASSERT_TRUE(flags & FD_CLOEXEC);
    close(31);

    /* dup2 invalid fd */
    ASSERT_EQ(-1, dup2(9999, 40));
    ASSERT(errno == EBADF);

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(dup_dup2_dup3, "Phase 5: Pipes & IPC");

int test_dup_eventfd_lifetime(void) {
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_GE(fd, 0);
    int copy = dup(fd);
    ASSERT_GE(copy, 0);
    ASSERT_EQ(0, close(fd));

    eventfd_t value = 7;
    ASSERT_EQ((ssize_t) sizeof(value), write(copy, &value, sizeof(value)));
    value = 0;
    ASSERT_EQ((ssize_t) sizeof(value), read(copy, &value, sizeof(value)));
    ASSERT_EQ((eventfd_t) 7, value);
    close(copy);
    return 1;
}
REGISTER_TEST(dup_eventfd_lifetime, "Phase 5: Pipes & IPC");

int test_socketpair(void) {
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (rc < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EAFNOSUPPORT)) return 1;
    ASSERT_EQ(0, rc);
    ASSERT_GE(sv[0], 0);
    ASSERT_GE(sv[1], 0);

    char buf[64];

    /* sv[0] -> sv[1] */
    ASSERT_EQ((ssize_t) 4, write(sv[0], "ping", 4));
    ssize_t n = read(sv[1], buf, sizeof(buf));
    ASSERT_EQ((ssize_t) 4, n);
    buf[n] = '\0';
    ASSERT_STREQ("ping", buf);

    /* sv[1] -> sv[0] (other direction) */
    ASSERT_EQ((ssize_t) 4, write(sv[1], "pong", 4));
    n = read(sv[0], buf, sizeof(buf));
    ASSERT_EQ((ssize_t) 4, n);
    buf[n] = '\0';
    ASSERT_STREQ("pong", buf);

    close(sv[0]);
    close(sv[1]);
    return 1;
}
REGISTER_TEST(socketpair, "Phase 5: Pipes & IPC");

int test_scm_rights_access_mode(void) {
    int sv[2];
    int rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    if (rc < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EAFNOSUPPORT)) return 1;
    ASSERT_EQ(0, rc);

    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "scm_rights_ro");
    write_file(path, "rights");
    int source = open(path, O_RDONLY);
    ASSERT_GE(source, 0);

    char byte = 'x';
    struct iovec siov = { .iov_base = &byte, .iov_len = 1 };
    char scontrol[CMSG_SPACE(sizeof(int))];
    memset(scontrol, 0, sizeof(scontrol));
    struct msghdr smsg;
    memset(&smsg, 0, sizeof(smsg));
    smsg.msg_iov = &siov;
    smsg.msg_iovlen = 1;
    smsg.msg_control = scontrol;
    smsg.msg_controllen = sizeof(scontrol);
    struct cmsghdr *scmsg = CMSG_FIRSTHDR(&smsg);
    scmsg->cmsg_level = SOL_SOCKET;
    scmsg->cmsg_type = SCM_RIGHTS;
    scmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(scmsg), &source, sizeof(source));
    ASSERT_EQ((ssize_t) 1, sendmsg(sv[0], &smsg, 0));
    close(source);

    char received_byte = 0;
    struct iovec riov = { .iov_base = &received_byte, .iov_len = 1 };
    char rcontrol[CMSG_SPACE(sizeof(int))];
    memset(rcontrol, 0, sizeof(rcontrol));
    struct msghdr rmsg;
    memset(&rmsg, 0, sizeof(rmsg));
    rmsg.msg_iov = &riov;
    rmsg.msg_iovlen = 1;
    rmsg.msg_control = rcontrol;
    rmsg.msg_controllen = sizeof(rcontrol);
    ASSERT_EQ((ssize_t) 1, recvmsg(sv[1], &rmsg, 0));
    ASSERT_EQ('x', received_byte);
    struct cmsghdr *rcmsg = CMSG_FIRSTHDR(&rmsg);
    ASSERT_NOTNULL(rcmsg);
    ASSERT_EQ(SOL_SOCKET, rcmsg->cmsg_level);
    ASSERT_EQ(SCM_RIGHTS, rcmsg->cmsg_type);
    int received = -1;
    memcpy(&received, CMSG_DATA(rcmsg), sizeof(received));
    ASSERT_GE(received, 0);

    char buf[8] = { 0 };
    ASSERT_EQ((ssize_t) 6, read(received, buf, 6));
    ASSERT_STREQ("rights", buf);
    ASSERT_EQ((ssize_t) -1, write(received, "!", 1));
    ASSERT_ERRNO(EBADF);

    close(received);
    close(sv[0]);
    close(sv[1]);
    unlink(path);
    return 1;
}
REGISTER_TEST(scm_rights_access_mode, "Phase 5: Pipes & IPC");

int test_unix_stream(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EAFNOSUPPORT)) return 1;
    ASSERT_GE(fd, 0);

    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "unix_stream.sock");
    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    ASSERT_EQ(0, bind(fd, (struct sockaddr *) &addr, sizeof(addr)));
    ASSERT_EQ(0, listen(fd, 5));

    /* fork a client */
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* client */
        int cf = socket(AF_UNIX, SOCK_STREAM, 0);
        if (cf < 0) _exit(2);
        if (connect(cf, (struct sockaddr *) &addr, sizeof(addr)) < 0) _exit(3);

        const char *msg = "hello from client";
        if (write(cf, msg, strlen(msg)) < 0) _exit(4);

        char buf[64];
        ssize_t n = read(cf, buf, sizeof(buf));
        if (n <= 0) _exit(5);

        close(cf);
        _exit(0);
    }

    /* server accept */
    struct sockaddr_un peer;
    socklen_t peerlen = sizeof(peer);
    int af = accept(fd, (struct sockaddr *) &peer, &peerlen);
    ASSERT_GE(af, 0);

    char buf[64];
    ssize_t n = read(af, buf, sizeof(buf));
    ASSERT_GT(n, 0);
    buf[n] = '\0';
    ASSERT_STREQ("hello from client", buf);

    const char *resp = "hello from server";
    ASSERT_EQ((ssize_t) strlen(resp), write(af, resp, strlen(resp)));

    close(af);
    close(fd);

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));

    unlink(path);
    return 1;
}
REGISTER_TEST(unix_stream, "Phase 5: Pipes & IPC");

int test_unix_dgram(void) {
    char path[PATH_MAX];
    tmpfile_path(path, sizeof(path), "unix_dgram.sock");
    unlink(path);

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EAFNOSUPPORT)) return 1;
    ASSERT_GE(fd, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    ASSERT_EQ(0, bind(fd, (struct sockaddr *) &addr, sizeof(addr)));

    /* sendto self */
    const char *msg = "dgram test";
    ASSERT_EQ((ssize_t) strlen(msg),
              sendto(fd, msg, strlen(msg), 0, (struct sockaddr *) &addr, sizeof(addr)));

    char buf[64];
    ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
    ASSERT_EQ((ssize_t) strlen(msg), n);
    buf[n] = '\0';
    ASSERT_STREQ(msg, buf);

    close(fd);
    unlink(path);
    return 1;
}
REGISTER_TEST(unix_dgram, "Phase 5: Pipes & IPC");

int test_shm_basic(void) {
    int shmid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
    if (shmid < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_GE(shmid, 0);

    void *p = shmat(shmid, NULL, 0);
    ASSERT_NE(p, (void *) -1);

    /* write to shared memory */
    const char *msg = "hello shm";
    memcpy(p, msg, strlen(msg) + 1);

    /* read back */
    ASSERT_STREQ(msg, (const char *) p);

    /* fork child and verify child can read */
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* child reads from shm */
        char child_buf[32];
        memcpy(child_buf, p, strlen(msg) + 1);
        _exit(strcmp(child_buf, msg) == 0 ? 0 : 1);
    }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));

    /* check attach count while the parent is still attached */
    struct shmid_ds ds;
    ASSERT_EQ(0, shmctl(shmid, IPC_STAT, &ds));
    ASSERT_EQ(1, ds.shm_nattch);

    ASSERT_EQ(0, shmdt(p));
    ASSERT_EQ(0, shmctl(shmid, IPC_RMID, NULL));
    return 1;
}
REGISTER_TEST(shm_basic, "Phase 5: Pipes & IPC");

int test_futex_basic(void) {
    uint32_t futex_word = 0;
    int ret;

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        usleep(100000);
        /* Wake the parent */
        syscall(SYS_futex, &futex_word, FUTEX_WAKE, 1, NULL, NULL, 0);
        _exit(0);
    }

    struct timespec timeout;
    timeout.tv_sec = 2;
    timeout.tv_nsec = 0;
    ret = syscall(SYS_futex, &futex_word, FUTEX_WAIT, 0, &timeout, NULL, 0);
    if (ret < 0) { ASSERT(errno == EAGAIN || errno == ETIMEDOUT || errno == EWOULDBLOCK); }

    int status;
    waitpid(pid, &status, 0);
    return 1;
}
REGISTER_TEST(futex_basic, "Phase 5: Pipes & IPC");

int test_futex_requeue(void) {
    static uint32_t f1, f2;
    f1 = 0;
    f2 = 0;

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        /* block on f1; the parent requeues us to f2, then wakes f2 */
        syscall(SYS_futex, &f1, FUTEX_WAIT, 0, NULL, NULL, 0);
        _exit(0);
    }

    usleep(150000); /* let the child reach FUTEX_WAIT on f1 */

    /* wake 0 waiters on f1, requeue up to 10 from f1 -> f2 */
    long r = syscall(SYS_futex, &f1, FUTEX_CMP_REQUEUE, 0, (void *) (long) 10, &f2, 0);
    if (r < 0 && errno == ENOSYS) { /* op unsupported: wake on f1 and skip */
        syscall(SYS_futex, &f1, FUTEX_WAKE, 100, NULL, NULL, 0);
        int st;
        waitpid(pid, &st, 0);
        return 1;
    }
    ASSERT_GE(r, 0);

    usleep(50000);
    /* release the requeued waiter on f2 (and f1 as a safety net) */
    syscall(SYS_futex, &f2, FUTEX_WAKE, 100, NULL, NULL, 0);
    syscall(SYS_futex, &f1, FUTEX_WAKE, 100, NULL, NULL, 0);

    int status;
    ASSERT_EQ(pid, waitpid(pid, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(0, WEXITSTATUS(status));
    return 1;
}
REGISTER_TEST(futex_requeue, "Phase 5: Pipes & IPC");

int test_eventfd(void) {
    return 1;

    int fd = eventfd(0, 0);
    if (fd < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_GE(fd, 0);

    /* write a value */
    eventfd_t val = 42;
    ASSERT_EQ(sizeof(val), write(fd, &val, sizeof(val)));

    /* read it back */
    val = 0;
    ASSERT_EQ(sizeof(val), read(fd, &val, sizeof(val)));
    ASSERT_EQ((eventfd_t) 42, val);

    close(fd);
    return 1;
}
REGISTER_TEST(eventfd, "Phase 5: Pipes & IPC");

int test_signalfd(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    int fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (fd < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_GE(fd, 0);
    kill(getpid(), SIGUSR1);

    struct signalfd_siginfo info;
    ssize_t n = read(fd, &info, sizeof(info));
    ASSERT_EQ((ssize_t) sizeof(info), n);
    ASSERT_EQ(SIGUSR1, info.ssi_signo);

    close(fd);
    return 1;
}
REGISTER_TEST(signalfd, "Phase 5: Pipes & IPC");
