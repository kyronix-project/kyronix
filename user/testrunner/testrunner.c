#include "test_harness.h"

#ifndef TEST_CASE_TIMEOUT
#define TEST_CASE_TIMEOUT 30
#endif

#define TEST_SKIP_MAX 64
#define TEST_SKIP_LEN 128

static int total = 0;
static int passed = 0;
static int failed = 0;
static int skipped = 0;

static int shard_index = 0;
static int shard_count = 1;
static char skip_patterns[TEST_SKIP_MAX][TEST_SKIP_LEN];
static int skip_count = 0;
static volatile sig_atomic_t sandbox_timed_out = 0;

char tmpdir[256];

int failure_pipe[2] = { -1, -1 };

#define MAX_FAILURES 64

static struct {
    const char *name;
    char detail[256];
} failures[MAX_FAILURES];
static int nfailures = 0;

test_entry_t test_registry[MAX_TESTS];
int test_count = 0;
int test_registry_overflow = 0;

static const char *cur_phase = NULL;
static int ph_total = 0;
static int ph_passed = 0;
static int ph_failed = 0;
static struct timespec phase_start;

static long elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (b->tv_sec - a->tv_sec) * 1000 + (b->tv_nsec - a->tv_nsec) / 1000000;
}

static void load_controls(void) {
    FILE *f = fopen("/test-shard", "r");
    if (f) {
        int n = 0;
        int m = 0;
        if (fscanf(f, "%d/%d", &n, &m) == 2 && m > 0 && n >= 0 && n < m) {
            shard_index = n;
            shard_count = m;
        }
        fclose(f);
    }

    f = fopen("/test-skip", "r");
    if (!f) return;

    char line[TEST_SKIP_LEN];
    while (skip_count < TEST_SKIP_MAX && fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r' ||
                           line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[--len] = '\0';
        }
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;
        snprintf(skip_patterns[skip_count], TEST_SKIP_LEN, "%s", p);
        skip_count++;
    }
    fclose(f);
}

static int test_skipped(const char *name, const char *phase) {
    for (int i = 0; i < skip_count; i++) {
        if (strstr(name, skip_patterns[i]) || strstr(phase, skip_patterns[i])) return 1;
    }
    return 0;
}

static void phase_summary(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "  " ANSI_CYAN "--- %s: %d/%d PASS, %d FAIL, %ldms" ANSI_RESET "\n", cur_phase,
            ph_passed, ph_total, ph_failed, elapsed_ms(&phase_start, &now));
}

static void phase_begin(const char *phase) {
    if (cur_phase) phase_summary();
    cur_phase = phase;
    ph_total = ph_passed = ph_failed = 0;
    clock_gettime(CLOCK_MONOTONIC, &phase_start);
    fprintf(stderr, "\n" ANSI_CYAN "[%s]" ANSI_RESET "\n", phase);
}

static void sandbox_alarm_handler(int sig) {
    (void) sig;
    sandbox_timed_out = 1;
}

static int run_sandbox(const char *test_name, int (*test_fn)(void)) {
    int p[2];
    if (pipe(p) < 0) {
        p[0] = -1;
        p[1] = -1;
    }

    if (p[1] >= 0) fcntl(p[1], F_SETFD, FD_CLOEXEC);
    failure_pipe[0] = p[0];
    failure_pipe[1] = p[1];

    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        failure_pipe[0] = failure_pipe[1] = -1;
        if (!setup_tmpdir()) return TEST_FAIL;
        int r = test_fn();
        cleanup_tmpdir();
        return r;
    }

    if (pid == 0) {
        close(p[0]);
        if (!setup_tmpdir()) _exit(TEST_FAIL);

        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }

        int r = test_fn();
        cleanup_tmpdir();
        close(p[1]);
        _exit(r);
    }

    close(p[1]);
    failure_pipe[1] = -1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sandbox_alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);

    sandbox_timed_out = 0;
    alarm(TEST_CASE_TIMEOUT);

    int status = 0;
    pid_t ret;
    for (;;) {
        ret = waitpid(pid, &status, 0);
        if (ret == pid) break;
        if (ret < 0 && errno == EINTR && !sandbox_timed_out) continue;
        break;
    }
    alarm(0);

    int timed_out = sandbox_timed_out && ret != pid;
    if (timed_out) {
        kill(pid, SIGKILL);
        for (int i = 0; i < 100; i++) {
            pid_t r = waitpid(pid, &status, WNOHANG);
            if (r == pid || r < 0) break;
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        if (nfailures < MAX_FAILURES) {
            failures[nfailures].name = test_name;
            snprintf(failures[nfailures].detail, sizeof(failures[nfailures].detail),
                     "timed out after %ds", TEST_CASE_TIMEOUT);
            nfailures++;
        }
    } else if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }

    ssize_t n = 0;
    char buf[256];
    if (!timed_out) {
        do { n = read(p[0], buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
        if (n > 0) {
            buf[n] = '\0';
            if (nfailures < MAX_FAILURES) {
                failures[nfailures].name = test_name;
                size_t len = strlen(buf);
                if (len > 0 && buf[len - 1] == '\n') buf[--len] = '\0';
                size_t cp = len < sizeof(failures[nfailures].detail) - 1 ?
                                len :
                                sizeof(failures[nfailures].detail) - 1;
                memcpy(failures[nfailures].detail, buf, cp);
                failures[nfailures].detail[cp] = '\0';
                nfailures++;
            }
        }
    }
    close(p[0]);
    failure_pipe[0] = -1;

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return TEST_FAIL;
}

int test_pipe_dup2_exec(void) {
    int p[2];
    if (pipe(p) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) return 0;

    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        execlp("ls", "ls", "/bin", NULL);
        _exit(127);
    }

    close(p[1]);
    char buf[256];
    ssize_t tot = 0, n;
    while (1) {
        n = read(p[0], buf + tot, sizeof(buf) - tot);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        tot += n;
    }
    close(p[0]);

    int status;
    waitpid(pid, &status, 0);

    return tot > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
REGISTER_TEST(pipe_dup2_exec, "Infrastructure");

int test_ls_grep_pipeline(void) {
    int p1[2], p2[2];
    if (pipe(p1) < 0) return 0;
    if (pipe(p2) < 0) {
        close(p1[0]);
        close(p1[1]);
        return 0;
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        close(p1[0]);
        close(p1[1]);
        close(p2[0]);
        close(p2[1]);
        return 0;
    }

    if (pid1 == 0) {
        close(p1[0]);
        close(p2[0]);
        close(p2[1]);
        dup2(p1[1], STDOUT_FILENO);
        close(p1[1]);
        execlp("ls", "ls", "/bin", NULL);
        _exit(127);
    }

    pid_t pid2 = fork();
    if (pid2 < 0) {
        close(p1[0]);
        close(p1[1]);
        close(p2[0]);
        close(p2[1]);
        return 0;
    }

    if (pid2 == 0) {
        close(p1[1]);
        close(p2[0]);
        dup2(p1[0], STDIN_FILENO);
        close(p1[0]);
        dup2(p2[1], STDOUT_FILENO);
        close(p2[1]);
        execlp("grep", "grep", "ksh", NULL);
        _exit(127);
    }

    close(p1[0]);
    close(p1[1]);
    close(p2[1]);

    char buf[256];
    ssize_t n;
    do { n = read(p2[0], buf, sizeof(buf) - 1); } while (n < 0 && errno == EINTR);
    close(p2[0]);
    buf[n > 0 ? n : 0] = '\0';

    int status;
    waitpid(pid2, &status, 0);
    waitpid(pid1, &status, 0);

    return strstr(buf, "ksh") != NULL;
}
REGISTER_TEST(ls_grep_pipeline, "Infrastructure");

int test_grep_o(void) {
    int p1[2], p2[2];
    if (pipe(p1) < 0 || pipe(p2) < 0) return 0;

    pid_t pid1 = fork();
    if (pid1 < 0) return 0;
    if (pid1 == 0) {
        close(p1[0]);
        close(p2[0]);
        close(p2[1]);
        dup2(p1[1], STDOUT_FILENO);
        close(p1[1]);
        execlp("echo", "echo", "fetch", NULL);
        _exit(127);
    }

    pid_t pid2 = fork();
    if (pid2 < 0) return 0;
    if (pid2 == 0) {
        close(p1[1]);
        close(p2[0]);
        dup2(p1[0], STDIN_FILENO);
        close(p1[0]);
        dup2(p2[1], STDOUT_FILENO);
        close(p2[1]);
        execlp("grep", "grep", "-o", "fetch", NULL);
        _exit(127);
    }

    close(p1[0]);
    close(p1[1]);
    close(p2[1]);

    char buf[64];
    ssize_t tot = 0, n;
    while (1) {
        n = read(p2[0], buf + tot, sizeof(buf) - 1 - tot);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        tot += n;
    }
    close(p2[0]);
    buf[tot > 0 ? tot : 0] = '\0';

    int status;
    waitpid(pid2, &status, 0);
    waitpid(pid1, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && strcmp(buf, "fetch\n") == 0;
}
REGISTER_TEST(grep_o, "Infrastructure");

int test_tiocgwinsz(void) {
    struct winsize ws;
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0) return 0;
    int ret = ioctl(fd, TIOCGWINSZ, &ws);
    close(fd);
    if (ret < 0) return 0;
    return ws.ws_row > 0 && ws.ws_col > 0;
}
REGISTER_TEST(tiocgwinsz, "Infrastructure");

int test_exec_fail(void) {
    pid_t pid = fork();
    if (pid < 0) return 0;

    if (pid == 0) {
        execlp("nonexistent-binary", "nonexistent-binary", NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 127;
}
REGISTER_TEST(exec_fail, "Infrastructure");

int test_basic_syscalls(void) {
    pid_t pid = getpid();
    pid_t ppid = getppid();
    uid_t uid = getuid();
    return pid > 0 && ppid >= 0 && uid == 0;
}
REGISTER_TEST(basic_syscalls, "Infrastructure");

int main(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }

    setenv("PATH", "/bin", 1);
    setenv("HOME", "/root", 1);

    fprintf(stderr, ANSI_CYAN "Kyronix Test Runner" ANSI_RESET "\n");
    fprintf(stderr, "--------------------\n");

    load_controls();

    fprintf(stderr, ANSI_CYAN "  kmemleak: %s" ANSI_RESET "\n",
            access("/proc/kmemleak", R_OK) == 0 ? "available (full scan skipped)" :
                                                  "not available");
    if (shard_count > 1)
        fprintf(stderr, ANSI_CYAN "  shard: %d/%d" ANSI_RESET "\n", shard_index, shard_count);
    if (skip_count > 0)
        fprintf(stderr, ANSI_CYAN "  skip patterns: %d" ANSI_RESET "\n", skip_count);
    if (test_registry_overflow)
        fprintf(stderr, ANSI_RED "  WARNING: %d test(s) dropped (MAX_TESTS=%d)" ANSI_RESET "\n",
                test_registry_overflow, MAX_TESTS);
    fprintf(stderr, "\n");

    struct timespec suite_start;
    struct timespec suite_end;
    clock_gettime(CLOCK_MONOTONIC, &suite_start);

    for (int i = 0; i < test_count; i++) {
        test_entry_t *e = &test_registry[i];

        if (shard_count > 1 && (i % shard_count) != shard_index) {
            skipped++;
            continue;
        }
        if (test_skipped(e->name, e->phase)) {
            skipped++;
            continue;
        }

        if (!cur_phase || strcmp(cur_phase, e->phase) != 0) phase_begin(e->phase);

        fflush(stderr);
        fprintf(stderr, "  %-30s ", e->name);
        fflush(stderr);

        struct timespec test_start;
        struct timespec test_end;
        int failures_before = nfailures;
        clock_gettime(CLOCK_MONOTONIC, &test_start);
        int result = run_sandbox(e->name, e->func);
        clock_gettime(CLOCK_MONOTONIC, &test_end);
        long test_ms = elapsed_ms(&test_start, &test_end);

        ph_total++;
        if (result == TEST_PASS) {
            passed++;
            ph_passed++;
            fprintf(stderr, ANSI_GREEN "PASS" ANSI_RESET " %ldms\n", test_ms);
        } else {
            failed++;
            ph_failed++;
            fprintf(stderr, ANSI_RED "FAIL" ANSI_RESET " %ldms", test_ms);
            if (nfailures > failures_before)
                fprintf(stderr, " (%s)", failures[nfailures - 1].detail);
            fprintf(stderr, "\n");
        }
        total++;
    }

    if (cur_phase) phase_summary();

    clock_gettime(CLOCK_MONOTONIC, &suite_end);
    fprintf(stderr, "\n" ANSI_CYAN "RESULT:" ANSI_RESET " %d/%d PASS, %d FAIL, %d SKIPPED, %ldms",
            passed, total, failed, skipped, elapsed_ms(&suite_start, &suite_end));

    fprintf(stderr, "\n");

    if (nfailures > 0) {
        fprintf(stderr, "\n" ANSI_RED "FAILED TESTS:" ANSI_RESET "\n");
        for (int i = 0; i < nfailures; i++)
            fprintf(stderr, "  %-28s %s\n", failures[i].name, failures[i].detail);
    }

    cleanup_tmpdir();

    if (failed == 0 && test_registry_overflow == 0) {
        fprintf(stderr, ANSI_GREEN "ALL TESTS PASSED" ANSI_RESET "\n");
    } else {
        fprintf(stderr, ANSI_RED "SOME TESTS FAILED" ANSI_RESET "\n");
    }

    fflush(stderr);
    reboot(LINUX_REBOOT_CMD_RESTART);
}
