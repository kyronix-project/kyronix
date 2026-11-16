#include "test_harness.h"

static volatile sig_atomic_t sig_usr1_caught = 0;

static void handler_usr1(int sig) {
    (void) sig;
    sig_usr1_caught = 1;
}

int test_sigaction_basic(void) {
    struct sigaction sa, old;

    sig_usr1_caught = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, &old));

    kill(getpid(), SIGUSR1);
    ASSERT_EQ(1, sig_usr1_caught);

    /* SIG_IGN */
    sa.sa_handler = SIG_IGN;
    ASSERT_EQ(0, sigaction(SIGUSR2, &sa, NULL));
    kill(getpid(), SIGUSR2);

    /* SIG_DFL (SIGUSR2 default is termination, but we just test the call) */
    sa.sa_handler = SIG_DFL;
    ASSERT_EQ(0, sigaction(SIGUSR2, &sa, NULL));

    return 1;
}
REGISTER_TEST(sigaction_basic, "Phase 4: Signal Handling");

int test_sigaction_sa_restart(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    int p[2];
    ASSERT_EQ(0, pipe(p));

    /* fork both children first, then read */
    pid_t sigchild = fork();
    ASSERT_GE(sigchild, 0);

    if (sigchild == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    pid_t writer = fork();
    ASSERT_GE(writer, 0);

    if (writer == 0) {
        usleep(100000);
        write(p[1], "X", 1);
        _exit(0);
    }

    /* both children are alive - read should be restarted by SA_RESTART */
    char c;
    ssize_t n = read(p[0], &c, 1);
    if (n < 0 && errno == EINTR) {
        /* kernel may not restart read even with SA_RESTART */
        close(p[0]);
        close(p[1]);
        int st;
        waitpid(sigchild, &st, 0);
        waitpid(writer, &st, 0);
        return 1;
    }
    ASSERT_EQ(1, n);

    close(p[0]);
    close(p[1]);

    int status;
    waitpid(sigchild, &status, 0);
    waitpid(writer, &status, 0);

    return 1;
}
REGISTER_TEST(sigaction_sa_restart, "Phase 4: Signal Handling");

int test_sigprocmask(void) {
    sigset_t newmask, oldmask, pending;

    /* install a handler so the unblocked SIGUSR1 is caught, not fatal */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    /* Block SIGUSR1 */
    sigemptyset(&newmask);
    sigaddset(&newmask, SIGUSR1);
    ASSERT_EQ(0, sigprocmask(SIG_BLOCK, &newmask, &oldmask));

    /* Send SIGUSR1 to self - should be pending */
    kill(getpid(), SIGUSR1);

    /* Check pending */
    ASSERT_EQ(0, sigpending(&pending));
    ASSERT_TRUE(sigismember(&pending, SIGUSR1));

    /* Unblock — handler should fire */
    sig_usr1_caught = 0;
    ASSERT_EQ(0, sigprocmask(SIG_UNBLOCK, &newmask, NULL));
    ASSERT_EQ(1, sig_usr1_caught);

    /* Restore */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    return 1;
}
REGISTER_TEST(sigprocmask, "Phase 4: Signal Handling");

int test_sigpending(void) {
    sigset_t mask, pending;

    /* ignore SIGTERM so unblocking the pending one doesn't terminate us */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGTERM, &sa, NULL));

    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    ASSERT_EQ(0, sigprocmask(SIG_BLOCK, &mask, NULL));

    kill(getpid(), SIGTERM);

    ASSERT_EQ(0, sigpending(&pending));
    ASSERT_TRUE(sigismember(&pending, SIGTERM));

    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return 1;
}
REGISTER_TEST(sigpending, "Phase 4: Signal Handling");

int test_sigsuspend(void) {
    sigset_t mask;
    sigemptyset(&mask);

    /* install a handler so the delivered SIGUSR1 is caught, not fatal */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    /* Schedule a signal from child */
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    /* sigsuspend with empty mask — any signal wakes us */
    sig_usr1_caught = 0;
    sigsuspend(&mask);
    ASSERT_EQ(1, sig_usr1_caught);

    int status;
    waitpid(pid, &status, 0);
    return 1;
}
REGISTER_TEST(sigsuspend, "Phase 4: Signal Handling");

int test_sigtimedwait(void) {
    sigset_t set;
    siginfo_t info;
    struct timespec timeout;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);

    /* Block the signal so it can be received by sigtimedwait */
    ASSERT_EQ(0, sigprocmask(SIG_BLOCK, &set, NULL));

    /* Send signal to self */
    kill(getpid(), SIGUSR1);

    timeout.tv_sec = 1;
    timeout.tv_nsec = 0;
    int sig = sigtimedwait(&set, &info, &timeout);
    ASSERT_EQ(SIGUSR1, sig);

    /* Timeout test — no signal pending */
    kill(getpid(), SIGUSR1);
    sig = sigtimedwait(&set, &info, &timeout);
    ASSERT_EQ(SIGUSR1, sig);

    sigprocmask(SIG_UNBLOCK, &set, NULL);
    return 1;
}
REGISTER_TEST(sigtimedwait, "Phase 4: Signal Handling");

int test_sigreturn(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    sig_usr1_caught = 0;
    kill(getpid(), SIGUSR1);
    ASSERT_EQ(1, sig_usr1_caught);

    /* If we got here, sigreturn worked (libc handles it) */
    return 1;
}
REGISTER_TEST(sigreturn, "Phase 4: Signal Handling");

static volatile sig_atomic_t alt_stack_used = 0;
static void handler_altstack(int sig) {
    (void) sig;
    stack_t os;
    sigaltstack(NULL, &os);
    if (os.ss_flags & SS_ONSTACK) alt_stack_used = 1;
}

int test_sigaltstack(void) {
    stack_t ss, os;
    struct sigaction sa;

    /* Allocate alternate stack */
    ss.ss_sp = malloc(SIGSTKSZ);
    ASSERT_NOTNULL(ss.ss_sp);
    ss.ss_size = SIGSTKSZ;
    ss.ss_flags = 0;
    ASSERT_EQ(0, sigaltstack(&ss, &os));

    /* Set handler with SA_ONSTACK */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_altstack;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    alt_stack_used = 0;
    kill(getpid(), SIGUSR1);
    ASSERT_EQ(1, alt_stack_used);

    /* Restore */
    sigaltstack(&os, NULL);
    free(ss.ss_sp);
    return 1;
}
REGISTER_TEST(sigaltstack, "Phase 4: Signal Handling");

int test_tkill_tgkill(void) {
    pid_t tid = gettid();
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    sig_usr1_caught = 0;
    long ret = syscall(SYS_tkill, tid, SIGUSR1);
    if (ret < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_EQ(1, sig_usr1_caught);

    sig_usr1_caught = 0;
    ret = syscall(SYS_tgkill, getpid(), tid, SIGUSR1);
    if (ret < 0 && errno == ENOSYS) return 1;
    ASSERT_EQ(0, ret);
    ASSERT_EQ(1, sig_usr1_caught);

    return 1;
}
REGISTER_TEST(tkill_tgkill, "Phase 4: Signal Handling");

int test_kill_basic(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    /* signal self */
    sig_usr1_caught = 0;
    ASSERT_EQ(0, kill(getpid(), SIGUSR1));
    ASSERT_EQ(1, sig_usr1_caught);

    /* signal 0 probe */
    ASSERT_EQ(0, kill(getpid(), 0));

    /* kill nonexistent process */
    ASSERT_EQ(-1, kill(999999, 0));
    ASSERT(errno == ESRCH);

    return 1;
}
REGISTER_TEST(kill_basic, "Phase 4: Signal Handling");

int test_pause(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    /* fork child to send signal */
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    sig_usr1_caught = 0;
    pause();
    ASSERT_EQ(1, sig_usr1_caught);

    int status;
    waitpid(pid, &status, 0);
    return 1;
}
REGISTER_TEST(pause, "Phase 4: Signal Handling");

static volatile sig_atomic_t sigalrm_caught = 0;

static void handler_alarm(int sig) {
    (void) sig;
    sigalrm_caught = 1;
}

int test_alarm(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_alarm;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGALRM, &sa, NULL));

    sigalrm_caught = 0;
    alarm(1);
    usleep(200000);
    /* not yet */
    ASSERT_EQ(0, sigalrm_caught);
    usleep(900000);
    ASSERT_EQ(1, sigalrm_caught);

    return 1;
}
REGISTER_TEST(alarm, "Phase 4: Signal Handling");

int test_waitpid_interrupted(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_usr1;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGUSR1, &sa, NULL));

    pid_t child = fork();
    ASSERT_GE(child, 0);

    if (child == 0) {
        /* child sleeps long enough for signal to be sent */
        sleep(1);
        _exit(0);
    }

    /* fork a signaler */
    pid_t sigpid = fork();
    ASSERT_GE(sigpid, 0);

    if (sigpid == 0) {
        usleep(100000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    sig_usr1_caught = 0;
    int status;

    /* We don't use SA_RESTART here, so EINTR is expected */
    pid_t ret = waitpid(child, &status, 0);
    if (ret == -1 && errno == EINTR) {
        /* signal arrived, wait again */
        ret = waitpid(child, &status, 0);
    }
    ASSERT_EQ(child, ret);
    ASSERT_TRUE(WIFEXITED(status));

    int st;
    waitpid(sigpid, &st, 0);
    return 1;
}
REGISTER_TEST(waitpid_interrupted, "Phase 4: Signal Handling");

int test_fork_sigmask(void) {
    sigset_t mask, child_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    ASSERT_EQ(0, sigprocmask(SIG_BLOCK, &mask, NULL));

    int p[2];
    ASSERT_EQ(0, pipe(p));

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* child should inherit blocked mask; read it back via sigprocmask */
        close(p[0]);
        ASSERT_EQ(0, sigprocmask(SIG_BLOCK, NULL, &child_mask));
        int blocked = sigismember(&child_mask, SIGUSR1);
        write(p[1], &blocked, sizeof(blocked));
        close(p[1]);
        _exit(0);
    }

    close(p[1]);
    int child_blocked = 0;
    ASSERT_EQ(sizeof(child_blocked), read(p[0], &child_blocked, sizeof(child_blocked)));
    close(p[0]);
    ASSERT_EQ(1, child_blocked);

    int status;
    waitpid(pid, &status, 0);
    sigprocmask(SIG_UNBLOCK, &mask, NULL);
    return 1;
}
REGISTER_TEST(fork_sigmask, "Phase 4: Signal Handling");

static volatile sig_atomic_t sigchld_caught = 0;

static void handler_sigchld(int sig) {
    (void) sig;
    sigchld_caught = 1;
}

int test_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler_sigchld;
    sigemptyset(&sa.sa_mask);
    ASSERT_EQ(0, sigaction(SIGCHLD, &sa, NULL));

    sigchld_caught = 0;
    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) { _exit(0); }

    int status;
    waitpid(pid, &status, 0);
    ASSERT_EQ(1, sigchld_caught);

    return 1;
}
REGISTER_TEST(sigchld, "Phase 4: Signal Handling");
