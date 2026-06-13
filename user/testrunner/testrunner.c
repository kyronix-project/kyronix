#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int total = 0;
static int passed = 0;

#define TEST(name)                                                                                 \
    do {                                                                                           \
        extern int test_##name(void);                                                              \
        total++;                                                                                   \
        fprintf(stderr, "  TEST %-25s ", #name);                                                   \
        if (test_##name()) {                                                                       \
            passed++;                                                                              \
            fprintf(stderr, "PASS\n");                                                             \
        } else {                                                                                   \
            fprintf(stderr, "FAIL\n");                                                             \
        }                                                                                          \
    } while (0)

static int test_pipe_dup2_exec(void)
{
    int p[2];
    if (pipe(p) < 0)
        return 0;

    pid_t pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        execlp("ls", "ls", "/bin", NULL);
        _exit(127);
    }

    close(p[1]);
    char buf[256];
    ssize_t total = 0, n;
    while ((n = read(p[0], buf + total, sizeof(buf) - total)) > 0)
        total += n;
    close(p[0]);

    int status;
    waitpid(pid, &status, 0);

    return total > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int test_ls_grep_pipeline(void)
{
    int p1[2], p2[2];
    if (pipe(p1) < 0)
        return 0;
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
        execlp("grep", "grep", "fetch", NULL);
        _exit(127);
    }

    close(p1[0]);
    close(p1[1]);
    close(p2[1]);

    char buf[256];
    ssize_t n = read(p2[0], buf, sizeof(buf) - 1);
    close(p2[0]);
    buf[n > 0 ? n : 0] = '\0';

    int status;
    waitpid(pid2, &status, 0);
    waitpid(pid1, &status, 0);

    return strstr(buf, "fetch") != NULL;
}

static int test_grep_o(void)
{
    int p1[2], p2[2];
    if (pipe(p1) < 0 || pipe(p2) < 0)
        return 0;

    pid_t pid1 = fork(); /* echo "fetch" */
    if (pid1 < 0)
        return 0;
    if (pid1 == 0) {
        close(p1[0]);
        close(p2[0]);
        close(p2[1]);
        dup2(p1[1], STDOUT_FILENO);
        close(p1[1]);
        execlp("echo", "echo", "fetch", NULL);
        _exit(127);
    }

    pid_t pid2 = fork(); /* grep -o fetch */
    if (pid2 < 0)
        return 0;
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
    ssize_t total = 0, n;
    while ((n = read(p2[0], buf + total, sizeof(buf) - 1 - total)) > 0)
        total += n;
    close(p2[0]);
    buf[total > 0 ? total : 0] = '\0';

    int status;
    waitpid(pid2, &status, 0);
    waitpid(pid1, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && strcmp(buf, "fetch\n") == 0;
}

static int test_tiocgwinsz(void)
{
    struct winsize ws;
    int fd = open("/dev/tty", O_RDWR);
    if (fd < 0)
        return 0;
    int ret = ioctl(fd, TIOCGWINSZ, &ws);
    close(fd);
    if (ret < 0)
        return 0;
    return ws.ws_row > 0 && ws.ws_col > 0;
}

static int test_exec_fail(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;

    if (pid == 0) {
        execlp("nonexistent-binary", "nonexistent-binary", NULL);
        _exit(127);
    }

    int status;
    waitpid(pid, &status, 0);

    return WIFEXITED(status) && WEXITSTATUS(status) == 127;
}

static int test_basic_syscalls(void)
{
    pid_t pid = getpid();
    pid_t ppid = getppid();
    uid_t uid = getuid();
    if (pid <= 0 || ppid < 0 || uid != 0)
        return 0;
    return 1;
}

int main(void)
{
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }

    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGALRM, SIG_IGN);

    setenv("PATH", "/bin", 1);

    fprintf(stderr, "Kyronix Test Runner\n");
    fprintf(stderr, "--------------------\n\n");

    TEST(pipe_dup2_exec);
    TEST(ls_grep_pipeline);
    TEST(grep_o);
    TEST(tiocgwinsz);
    TEST(exec_fail);
    TEST(basic_syscalls);

    fprintf(stderr, "\nRESULT: %d/%d passed\n", passed, total);

    if (passed == total) {
        fprintf(stderr, "ALL TESTS PASSED\n");
        fflush(stderr);
        /* reboot */
        __asm__ volatile("mov $169, %%rax; xor %%rdi, %%rdi; syscall" ::: "rax", "rdi");
    } else {
        fprintf(stderr, "SOME TESTS FAILED\n");
        fflush(stderr);
    }

    for (;;)
        pause();
}
