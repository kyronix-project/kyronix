#include "test_harness.h"
#include <pty.h>

/* A terminal must see EOF/HUP after the last slave handle closes. */
int test_pty_slave_hup(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_TRUE(master >= 0);
    ASSERT_EQ(0, grantpt(master));
    ASSERT_EQ(0, unlockpt(master));
    char *name = ptsname(master);
    ASSERT_TRUE(name != NULL);
    int slave = open(name, O_RDWR | O_NOCTTY);
    ASSERT_TRUE(slave >= 0);

    int ep = epoll_create1(0);
    ASSERT_TRUE(ep >= 0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP, .data.fd = master };
    ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_ADD, master, &ev));
    ASSERT_EQ(0, close(slave));

    struct epoll_event out;
    ASSERT_EQ(1, epoll_wait(ep, &out, 1, 1000));
    ASSERT_TRUE(out.events & EPOLLHUP);
    ASSERT_EQ(0, fcntl(master, F_SETFL, O_NONBLOCK));
    char byte;
    ASSERT_EQ(0, read(master, &byte, 1));

    close(ep);
    close(master);
    return 1;
}
REGISTER_TEST(pty_slave_hup, "Phase 7: I/O Multiplexing");

int test_forkpty_child_hup(void) {
    int master = -1;
    pid_t child = forkpty(&master, NULL, NULL, NULL);
    ASSERT_TRUE(child >= 0);
    if (child == 0) _exit(0);
    int status;
    ASSERT_EQ(child, waitpid(child, &status, 0));
    ASSERT_TRUE(WIFEXITED(status));

    int ep = epoll_create1(0);
    ASSERT_TRUE(ep >= 0);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLHUP, .data.fd = master };
    ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_ADD, master, &ev));
    struct epoll_event out;
    ASSERT_EQ(1, epoll_wait(ep, &out, 1, 1000));
    ASSERT_TRUE(out.events & EPOLLHUP);
    ASSERT_EQ(0, epoll_ctl(ep, EPOLL_CTL_DEL, master, NULL));
    ASSERT_EQ(0, epoll_wait(ep, &out, 1, 0));
    close(ep);
    close(master);
    return 1;
}
REGISTER_TEST(forkpty_child_hup, "Phase 7: I/O Multiplexing");

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

/*
 * libinput hands its clients a single epoll handle, which weston then watches
 * from its own event loop, so an epoll fd has to be pollable itself.
 */
int test_epoll_nested(void) {
    int p[2];
    ASSERT_EQ(0, pipe(p));
    int inner = epoll_create1(0);
    ASSERT_GE(inner, 0);
    int outer = epoll_create1(0);
    ASSERT_GE(outer, 0);

    struct epoll_event ev = { .events = EPOLLIN, .data = { .fd = p[0] } };
    ASSERT_EQ(0, epoll_ctl(inner, EPOLL_CTL_ADD, p[0], &ev));
    ev.data.fd = inner;
    ASSERT_EQ(0, epoll_ctl(outer, EPOLL_CTL_ADD, inner, &ev));

    struct epoll_event out[2];
    ASSERT_EQ(0, epoll_wait(outer, out, 2, 0));

    ASSERT_EQ((ssize_t) 1, write(p[1], "x", 1));
    ASSERT_EQ(1, epoll_wait(outer, out, 2, 1000));
    ASSERT_EQ(inner, out[0].data.fd);
    /* the inner set reports the real fd */
    ASSERT_EQ(1, epoll_wait(inner, out, 2, 0));
    ASSERT_EQ(p[0], out[0].data.fd);

    char c = 0;
    ASSERT_EQ((ssize_t) 1, read(p[0], &c, 1));
    ASSERT_EQ(0, epoll_wait(outer, out, 2, 0));

    /* libwayland dups the fd before watching it, so a dup of an epoll handle
       has to behave like the original */
    int dup_inner = dup(inner);
    ASSERT_GE(dup_inner, 0);
    ev.data.fd = dup_inner;
    ASSERT_EQ(0, epoll_ctl(outer, EPOLL_CTL_DEL, inner, &ev));
    ASSERT_EQ(0, epoll_ctl(outer, EPOLL_CTL_ADD, dup_inner, &ev));
    ASSERT_EQ((ssize_t) 1, write(p[1], "z", 1));
    ASSERT_EQ(1, epoll_wait(outer, out, 2, 1000));
    ASSERT_EQ(dup_inner, out[0].data.fd);
    ASSERT_EQ((ssize_t) 1, read(p[0], &c, 1));
    ASSERT_EQ(0, epoll_wait(outer, out, 2, 0));
    ASSERT_EQ(0, epoll_ctl(outer, EPOLL_CTL_DEL, dup_inner, &ev));
    close(dup_inner);
    ev.data.fd = inner;
    ASSERT_EQ(0, epoll_ctl(outer, EPOLL_CTL_ADD, inner, &ev));

    /* and a blocking wait must be woken when the inner fd becomes ready */
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        usleep(150000);
        write(p[1], "y", 1);
        _exit(0);
    }
    ASSERT_EQ(1, epoll_wait(outer, out, 2, 3000));
    ASSERT_EQ(inner, out[0].data.fd);

    int status = 0;
    waitpid(pid, &status, 0);
    close(inner);
    close(outer);
    close(p[0]);
    close(p[1]);
    return 1;
}
REGISTER_TEST(epoll_nested, "Phase 7: I/O Multiplexing");

static volatile sig_atomic_t winch_seen = 0;

static void winch_handler(int sig) {
    if (sig == SIGWINCH) winch_seen = 1;
}

/* A TUI redraws on SIGWINCH. If the kernel never sends it, a resized terminal
 * leaves the application drawing for the old geometry. The parent holds the
 * master: the signal goes to the pty's foreground process group, which the
 * child claims with TIOCSCTTY. */
int test_pty_tiocswinsz_sends_winch(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_TRUE(master >= 0);
    ASSERT_EQ(0, grantpt(master));
    ASSERT_EQ(0, unlockpt(master));
    char *name = ptsname(master);
    ASSERT_TRUE(name != NULL);

    int ready[2];
    ASSERT_EQ(0, pipe(ready));

    pid_t child = fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        /* Own session and process group first. TIOCSCTTY records the caller's
         * process group in the pty, and every pty signal (including the SIGHUP
         * on master close, whose default action is fatal) is then sent to that
         * whole group -- which, without this, is the test runner's own group
         * and would take the suite down with the child. */
        setsid();
        close(master);
        close(ready[0]);
        int slave = open(name, O_RDWR);
        if (slave < 0) _exit(1);
        /* Makes this the controlling terminal and puts us in its foreground
         * process group, which is what pty_send_sig targets. */
        ioctl(slave, TIOCSCTTY, 0);

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = winch_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGWINCH, &sa, NULL);

        char ok = 'x';
        ssize_t w = write(ready[1], &ok, 1);
        (void) w;
        for (int i = 0; i < 300 && !winch_seen; i++) usleep(20000);
        _exit(winch_seen ? 42 : 7);
    }

    close(ready[1]);
    char ack = 0;
    ssize_t r = read(ready[0], &ack, 1);
    (void) r;
    close(ready[0]);

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = 30;
    ws.ws_col = 100;
    ASSERT_EQ(0, ioctl(master, TIOCSWINSZ, &ws));

    int status = 0;
    int got = 0;
    for (int i = 0; i < 400; i++) {
        pid_t p = waitpid(child, &status, WNOHANG);
        if (p == child) {
            got = WIFEXITED(status) && WEXITSTATUS(status) == 42;
            break;
        }
        usleep(20000);
    }
    if (!got) {
        /* Best effort only: a child asleep in nanosleep cannot be killed, so a
         * blocking reap here would hang the suite instead of failing the test. */
        kill(child, SIGKILL);
        for (int i = 0; i < 50; i++) {
            if (waitpid(child, &status, WNOHANG) == child) break;
            usleep(20000);
        }
    }
    close(master);
    ASSERT_TRUE(got);
    return 1;
}
REGISTER_TEST(pty_tiocswinsz_sends_winch, "Phase 7: I/O Multiplexing");
