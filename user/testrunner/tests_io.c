#include "test_harness.h"

int test_poll_basic(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    struct pollfd fds[2];

    /* POLLIN on empty pipe */
    fds[0].fd = p[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    int ret = poll(fds, 1, 10);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_EQ(0, ret);

    /* Write, then POLLIN should fire */
    write(p[1], "x", 1);
    fds[0].fd = p[0];
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    ret = poll(fds, 1, 100);
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(fds[0].revents & POLLIN);

    /* POLLOUT on write end */
    fds[0].fd = p[1];
    fds[0].events = POLLOUT;
    fds[0].revents = 0;
    ret = poll(fds, 1, 0);
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(fds[0].revents & POLLOUT);

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(poll_basic, "Phase 7: I/O Multiplexing");

int test_poll_hup(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        close(p[0]);
        usleep(50000);
        close(p[1]);
        _exit(0);
    }

    close(p[1]);

    struct pollfd fd;
    fd.fd = p[0];
    fd.events = POLLIN;
    int ret = poll(&fd, 1, 5000);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        kill(pid, SIGKILL);
        int st;
        waitpid(pid, &st, 0);
        return 1;
    }
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(fd.revents & POLLHUP);

    close(p[0]);

    int status;
    waitpid(pid, &status, 0);
    return 1;
}
REGISTER_TEST(poll_hup, "Phase 7: I/O Multiplexing");

int test_poll_nval(void) {
    struct pollfd fd;
    fd.fd = 9999;
    fd.events = POLLIN;

    int ret = poll(&fd, 1, 0);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) return 1;
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(fd.revents & POLLNVAL);

    return 1;
}
REGISTER_TEST(poll_nval, "Phase 7: I/O Multiplexing");

int test_poll_nfds_limit(void) {
    struct pollfd fd = { .fd = -1, .events = 0, .revents = 0 };
    errno = 0;
    unsigned long overflowing_nfds = UINT64_MAX / sizeof(fd) + 1;
    ASSERT_EQ(-1, syscall(SYS_poll, &fd, overflowing_nfds, 0));
    ASSERT_ERRNO(EINVAL);
    return 1;
}
REGISTER_TEST(poll_nfds_limit, "Phase 7: I/O Multiplexing");

int test_ppoll(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGUSR1);

    struct pollfd fd;
    fd.fd = p[0];
    fd.events = POLLIN;

    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 50000000;

    /* ppoll should wait for data with sigmask applied */
    int ret = ppoll(&fd, 1, &timeout, &sigmask);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_EQ(0, ret);

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(ppoll, "Phase 7: I/O Multiplexing");

int test_select_basic(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000;

    int ret = select(p[0] + 1, &rfds, NULL, NULL, &tv);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_EQ(0, ret);

    /* Write, then select should fire */
    write(p[1], "x", 1);
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    ret = select(p[0] + 1, &rfds, NULL, NULL, &tv);
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(FD_ISSET(p[0], &rfds));

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(select_basic, "Phase 7: I/O Multiplexing");

int test_select_write(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(p[1], &wfds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    int ret = select(p[1] + 1, NULL, &wfds, NULL, &tv);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_EQ(1, ret);
    ASSERT_TRUE(FD_ISSET(p[1], &wfds));

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(select_write, "Phase 7: I/O Multiplexing");

int test_pselect6(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);

    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 10000000;

    sigset_t sigmask;
    sigemptyset(&sigmask);

    int ret = pselect(p[0] + 1, &rfds, NULL, NULL, &ts, &sigmask);
    if (ret < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_EQ(0, ret);

    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(pselect6, "Phase 7: I/O Multiplexing");

int test_epoll_basic(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    int epfd = epoll_create1(0);
    if (epfd < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }
    ASSERT_GE(epfd, 0);

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    ASSERT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev));

    /* epoll_wait with no data - timeout 10ms */
    struct epoll_event out;
    int n = epoll_wait(epfd, &out, 1, 10);
    ASSERT_EQ(0, n);

    /* Write, then epoll should fire */
    write(p[1], "x", 1);
    n = epoll_wait(epfd, &out, 1, 1000);
    ASSERT_EQ(1, n);
    ASSERT_EQ(EPOLLIN, out.events & EPOLLIN);

    close(epfd);
    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(epoll_basic, "Phase 7: I/O Multiplexing");

int test_epoll_et(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    int epfd = epoll_create1(0);
    if (epfd < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }

    /* Edge-triggered */
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = p[0];
    ASSERT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev));

    write(p[1], "abc", 3);

    struct epoll_event out;
    int n = epoll_wait(epfd, &out, 1, 1000);
    ASSERT_EQ(1, n);
    ASSERT_TRUE(out.events & EPOLLIN);

    /* Read all data */
    char buf[16];
    ASSERT_EQ(3, read(p[0], buf, sizeof(buf)));

    /* ET: should not fire again until more data arrives */
    n = epoll_wait(epfd, &out, 1, 100);
    ASSERT_EQ(0, n);

    /* Write more, should fire again */
    write(p[1], "d", 1);
    n = epoll_wait(epfd, &out, 1, 1000);
    ASSERT_EQ(1, n);
    ASSERT_TRUE(out.events & EPOLLIN);

    close(epfd);
    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(epoll_et, "Phase 7: I/O Multiplexing");

int test_epoll_oneshot(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    int epfd = epoll_create1(0);
    if (epfd < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLONESHOT;
    ev.data.fd = p[0];
    ASSERT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev));

    write(p[1], "x", 1);

    struct epoll_event out;
    int n = epoll_wait(epfd, &out, 1, 1000);
    ASSERT_EQ(1, n);
    ASSERT_TRUE(out.events & EPOLLIN);

    /* EPOLLONESHOT: should NOT fire again without re-arming */
    n = epoll_wait(epfd, &out, 1, 100);
    ASSERT_EQ(0, n);

    /* Re-arm */
    ev.events = EPOLLIN | EPOLLONESHOT;
    ASSERT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_MOD, p[0], &ev));
    n = epoll_wait(epfd, &out, 1, 100);
    ASSERT_EQ(1, n);
    ASSERT_TRUE(out.events & EPOLLIN);

    close(epfd);
    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(epoll_oneshot, "Phase 7: I/O Multiplexing");

int test_epoll_hup(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));

    int epfd = epoll_create1(0);
    if (epfd < 0 && (errno == ENOSYS || errno == ENOTSUP)) {
        close(p[0]);
        close(p[1]);
        return 1;
    }

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = p[0];
    ASSERT_EQ(0, epoll_ctl(epfd, EPOLL_CTL_ADD, p[0], &ev));

    close(p[1]);

    struct epoll_event out;
    int n = epoll_wait(epfd, &out, 1, 1000);
    ASSERT_EQ(1, n);
    ASSERT_TRUE(out.events & EPOLLHUP);

    close(epfd);
    close(p[0]);
    return 1;
}
REGISTER_TEST(epoll_hup, "Phase 7: I/O Multiplexing");
