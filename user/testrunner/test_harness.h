#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/shm.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#ifndef LINUX_REBOOT_CMD_RESTART
#define LINUX_REBOOT_CMD_RESTART 0x01234567
#endif

#ifndef ARCH_GET_FS
#define ARCH_GET_FS 0x1003
#endif
#ifndef ARCH_SET_FS
#define ARCH_SET_FS 0x1002
#endif

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif
#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif
#ifndef FUTEX_REQUEUE
#define FUTEX_REQUEUE 3
#endif
#ifndef FUTEX_CMP_REQUEUE
#define FUTEX_CMP_REQUEUE 4
#endif

#define ANSI_GREEN "\033[32m"
#define ANSI_RED "\033[31m"
#define ANSI_CYAN "\033[36m"
#define ANSI_RESET "\033[0m"

#define TEST_PASS 1
#define TEST_FAIL 0

extern int failure_pipe[2];

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "\n  " ANSI_RED "!" ANSI_RESET " %s:%d: %s\n", __FILE__, __LINE__,     \
                    #cond);                                                                        \
            if (failure_pipe[1] >= 0)                                                              \
                dprintf(failure_pipe[1], "%s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            return TEST_FAIL;                                                                      \
        }                                                                                          \
    } while (0)
#define ASSERT_EQ(a, b) ASSERT((a) == (b))
#define ASSERT_NE(a, b) ASSERT((a) != (b))
#define ASSERT_GT(a, b) ASSERT((a) > (b))
#define ASSERT_GE(a, b) ASSERT((a) >= (b))
#define ASSERT_LT(a, b) ASSERT((a) < (b))
#define ASSERT_LE(a, b) ASSERT((a) <= (b))
#define ASSERT_STREQ(a, b) ASSERT(strcmp((a), (b)) == 0)
#define ASSERT_STRNE(a, b) ASSERT(strcmp((a), (b)) != 0)
#define ASSERT_STREQN(a, b, n) ASSERT(strncmp((a), (b), (n)) == 0)
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOTNULL(p) ASSERT((p) != NULL)
#define ASSERT_TRUE(c) ASSERT((c) != 0)
#define ASSERT_FALSE(c) ASSERT((c) == 0)
#define ASSERT_ERRNO(e) ASSERT(errno == (e))

extern char tmpdir[256];

static inline int setup_tmpdir(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/test_%d", getpid());
    if (mkdir(tmpdir, 0777) < 0 && errno != EEXIST) return 0;
    return 1;
}

static inline void cleanup_tmpdir(void) {
    /* manually unlink all files in tmpdir, then remove the dir */
    DIR *d = opendir(tmpdir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char p[512];
            snprintf(p, sizeof(p), "%s/%s", tmpdir, e->d_name);
            unlink(p);
            rmdir(p);
        }
        closedir(d);
    }
    rmdir(tmpdir);
}

static inline int tmpfile_path(char *buf, size_t sz, const char *name) {
    return snprintf(buf, sz, "%s/%s", tmpdir, name);
}

static inline int write_file(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return 0;
    size_t len = strlen(content);
    ssize_t r = write(fd, content, len);
    close(fd);
    return (size_t) r == len;
}

static inline ssize_t read_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n >= 0) buf[n] = '\0';
    return n;
}

static inline int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static inline int capture_cmd(char *const argv[], char *buf, size_t bufsz) {
    int p[2];
    if (pipe(p) < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        close(p[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    ssize_t total = 0;
    while (total < (ssize_t) bufsz) {
        ssize_t n = read(p[0], buf + total, bufsz - 1 - total);
        if (n <= 0) break;
        total += n;
    }
    close(p[0]);
    buf[total > 0 ? total : 0] = '\0';
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static inline pid_t spawn_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static inline int wait_for(pid_t pid) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static inline int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0777) < 0 && errno != EEXIST) return -1;
    return 0;
}

typedef struct {
    const char *name;
    const char *phase;
    int (*func)(void);
} test_entry_t;

#define MAX_TESTS 256

extern test_entry_t test_registry[MAX_TESTS];
extern int test_count;

#define REGISTER_TEST(tname, phase_name)                                                           \
    static void __attribute__((constructor)) __reg_##tname(void) {                                 \
        if (test_count < MAX_TESTS) {                                                              \
            test_registry[test_count].name = #tname;                                               \
            test_registry[test_count].phase = phase_name;                                          \
            test_registry[test_count].func = test_##tname;                                         \
            test_count++;                                                                          \
        }                                                                                          \
    }

#endif /* TEST_HARNESS_H */
