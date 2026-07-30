#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pkg.h"

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stdout, "  ");
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_step(const char *step, const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s=>%s %s%s%s ", ANSI_CYAN, ANSI_RESET, ANSI_BOLD, step, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_done(const char *fmt, ...) {
    va_list ap;
    fprintf(stdout, "%s[*]%s ", ANSI_GREEN, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s[!]%s ", ANSI_YELLOW, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void dief(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s%serror:%s ", ANSI_RED, ANSI_BOLD, ANSI_RESET);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || (unsigned long) sz > PKG_MAX_LOCAL_FILE) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    if (len_out) *len_out = rd;
    return buf;
}

int write_text_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

int starts_with(const char *s, const char *p) {
    return strncmp(s, p, strlen(p)) == 0;
}

void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

void ensure_dir(const char *path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        dief("mkdir failed: %s", path);
    }
}

void ensure_dir_mode(const char *path, unsigned mode) {
    if (mkdir(path, (mode_t) mode) != 0 && errno != EEXIST)
        dief("mkdir failed: %s", path);
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        dief("unsafe directory path: %s", path);
    if (chmod(path, (mode_t) mode) != 0)
        dief("chmod failed: %s", path);
}

int valid_pkg_name(const char *name) {
    if (!name || !name[0]) return 0;
    size_t len = strlen(name);
    if (len > 127 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) name[i];
        if (!(isalnum(c) || c == '+' || c == '-' || c == '_' || c == '.')) return 0;
    }
    return 1;
}

int valid_repo_name(const char *name) {
    if (!name || !name[0] || strlen(name) > 127) return 0;
    for (const unsigned char *p = (const unsigned char *) name; *p; p++)
        if (*p < 0x20 || *p == 0x7f || *p == '[' || *p == ']' || *p == '=')
            return 0;
    return 1;
}

int valid_repo_url(const char *url) {
    if (!url || !starts_with(url, "http://") || strlen(url) > 511) return 0;
    for (const unsigned char *p = (const unsigned char *) url; *p; p++)
        if (*p <= 0x20 || *p == 0x7f) return 0;
    return url[7] != '\0';
}

static int path_has_dot_component(const char *path) {
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t) (p - start);
        if ((len == 1 && start[0] == '.') ||
            (len == 2 && start[0] == '.' && start[1] == '.'))
            return 1;
    }
    return 0;
}

int safe_managed_path(const char *path) {
    if (!path || path[0] != '/' || path_has_dot_component(path)) return 0;
    for (const unsigned char *p = (const unsigned char *) path; *p; p++)
        if (*p < 0x20 || *p == 0x7f) return 0;
    return starts_with(path, "/usr/") || starts_with(path, "/etc/") ||
           starts_with(path, "/var/lib/xkb/") || starts_with(path, "/root/.dillo/");
}

int remove_tree(const char *path) {
    struct stat st;
    if (!path || lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);

    /*
     * Reopen the directory after every deletion.  Kyronix compacts directory
     * entries on unlink, so continuing an existing readdir stream can skip
     * the entry that shifted into the just-removed slot.
     */
    for (;;) {
        DIR *dir = opendir(path);
        if (!dir) return -1;
        char name[256] = "";
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 ||
                strcmp(entry->d_name, "..") == 0)
                continue;
            size_t name_len = strlen(entry->d_name);
            if (name_len >= sizeof(name)) {
                closedir(dir);
                return -1;
            }
            memcpy(name, entry->d_name, name_len + 1);
            break;
        }
        if (closedir(dir) != 0) return -1;
        if (!name[0]) break;

        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, name);
        if (n < 0 || (size_t) n >= sizeof(child) || remove_tree(child) != 0)
            return -1;
    }
    return rmdir(path);
}

int confirm_prompt(const char *prompt) {
    fputs(prompt, stdout);
    fflush(stdout);

    struct termios saved;
    int restore = 0;
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &saved) == 0) {
        struct termios interactive = saved;
        interactive.c_lflag |= ECHO | ICANON;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &interactive) == 0) restore = 1;
    }

    char answer[16] = "";
    char *got = fgets(answer, sizeof(answer), stdin);
    if (restore) tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (!got) {
        fputc('\n', stdout);
        return 0;
    }
    if (!strchr(answer, '\n')) {
        int c;
        while ((c = fgetc(stdin)) != '\n' && c != EOF) {}
    }
    return answer[0] == '\n' || answer[0] == 'y' || answer[0] == 'Y';
}

static void sanitize_child_environment(void) {
    setenv("PATH", "/bin:/usr/bin", 1);
    unsetenv("ENV");
    unsetenv("BASH_ENV");
    unsetenv("CDPATH");
    unsetenv("IFS");
    unsetenv("LD_PRELOAD");
    unsetenv("LD_LIBRARY_PATH");
}

int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        sanitize_child_environment();
        if (strchr(argv[0], '/'))
            execv(argv[0], argv);
        else
            execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    return -1;
}

int run_cmd_in(const char *dir, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (dir && chdir(dir) != 0) _exit(1);
        sanitize_child_environment();
        if (strchr(argv[0], '/'))
            execv(argv[0], argv);
        else
            execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    return -1;
}

int run_cmd_input_file(const char *input_path, char *const argv[]) {
    int input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(input_fd);
        return -1;
    }
    if (pid == 0) {
        if (dup2(input_fd, STDIN_FILENO) < 0) _exit(1);
        close(input_fd);
        sanitize_child_environment();
        if (strchr(argv[0], '/'))
            execv(argv[0], argv);
        else
            execvp(argv[0], argv);
        _exit(127);
    }
    close(input_fd);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int run_pipeline_file_in(const char *dir, const char *input_path,
                         char *const producer[], char *const consumer[],
                         size_t max_output) {
    int input_fd = open(input_path, O_RDONLY);
    if (input_fd < 0) return -1;
    int producer_pipe[2], consumer_pipe[2];
    if (pipe(producer_pipe) != 0) {
        close(input_fd);
        return -1;
    }
    if (pipe(consumer_pipe) != 0) {
        close(input_fd);
        close(producer_pipe[0]);
        close(producer_pipe[1]);
        return -1;
    }

    pid_t producer_pid = fork();
    if (producer_pid < 0) {
        close(input_fd);
        close(producer_pipe[0]);
        close(producer_pipe[1]);
        close(consumer_pipe[0]);
        close(consumer_pipe[1]);
        return -1;
    }
    if (producer_pid == 0) {
        if (dup2(input_fd, STDIN_FILENO) < 0) _exit(1);
        if (dup2(producer_pipe[1], STDOUT_FILENO) < 0) _exit(1);
        close(input_fd);
        close(producer_pipe[0]);
        close(producer_pipe[1]);
        close(consumer_pipe[0]);
        close(consumer_pipe[1]);
        sanitize_child_environment();
        if (strchr(producer[0], '/'))
            execv(producer[0], producer);
        else
            execvp(producer[0], producer);
        _exit(127);
    }

    pid_t consumer_pid = fork();
    if (consumer_pid < 0) {
        close(input_fd);
        close(producer_pipe[0]);
        close(producer_pipe[1]);
        close(consumer_pipe[0]);
        close(consumer_pipe[1]);
        waitpid(producer_pid, NULL, 0);
        return -1;
    }
    if (consumer_pid == 0) {
        if (dir && chdir(dir) != 0) _exit(1);
        if (dup2(consumer_pipe[0], STDIN_FILENO) < 0) _exit(1);
        close(input_fd);
        close(producer_pipe[0]);
        close(producer_pipe[1]);
        close(consumer_pipe[0]);
        close(consumer_pipe[1]);
        sanitize_child_environment();
        if (strchr(consumer[0], '/'))
            execv(consumer[0], consumer);
        else
            execvp(consumer[0], consumer);
        _exit(127);
    }

    close(input_fd);
    close(producer_pipe[1]);
    close(consumer_pipe[0]);
    void (*old_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    int relay_ok = 1;
    size_t total = 0;
    unsigned char buffer[64U * 1024U];
    for (;;) {
        ssize_t got = read(producer_pipe[0], buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) {
            relay_ok = 0;
            break;
        }
        if (got == 0) break;
        if ((size_t) got > max_output - total) {
            relay_ok = 0;
            break;
        }
        total += (size_t) got;
        size_t offset = 0;
        while (offset < (size_t) got) {
            ssize_t sent = write(consumer_pipe[1], buffer + offset,
                                 (size_t) got - offset);
            if (sent < 0 && errno == EINTR) continue;
            if (sent <= 0) {
                relay_ok = 0;
                break;
            }
            offset += (size_t) sent;
        }
        if (!relay_ok) break;
    }
    close(producer_pipe[0]);
    close(consumer_pipe[1]);
    if (old_sigpipe != SIG_ERR)
        signal(SIGPIPE, old_sigpipe);
    int producer_status = 0;
    int consumer_status = 0;
    if (waitpid(producer_pid, &producer_status, 0) < 0)
        producer_status = -1;
    if (waitpid(consumer_pid, &consumer_status, 0) < 0)
        consumer_status = -1;
    return relay_ok && producer_status >= 0 && consumer_status >= 0 &&
                   WIFEXITED(producer_status) && WEXITSTATUS(producer_status) == 0 &&
                   WIFEXITED(consumer_status) && WEXITSTATUS(consumer_status) == 0 ?
               0 :
               -1;
}

int read_repos(RepoConfig *repos, int max) {
    size_t len = 0;
    char *txt = read_file(REPO_SOURCES_PATH, &len);
    if (!txt) return 0;

    int count = 0;
    char current_name[128] = "";
    char current_url[512] = "";
    int current_priority = 0;
    int has_section = 0;

    char *line = txt;
    while (*line && count < max) {
        while (*line == '\n' || *line == '\r') line++;
        if (!*line) break;

        char *eol = strchr(line, '\n');
        size_t llen = eol ? (size_t)(eol - line) : strlen(line);
        char lbuf[1024];
        if (llen >= sizeof(lbuf)) llen = sizeof(lbuf) - 1;
        memcpy(lbuf, line, llen);
        lbuf[llen] = '\0';
        line = eol ? eol + 1 : line + llen;

        size_t n = strlen(lbuf);
        while (n > 0 && (lbuf[n-1] == ' ' || lbuf[n-1] == '\t' || lbuf[n-1] == '\r')) lbuf[--n] = '\0';
        char *s = lbuf;
        while (*s == ' ' || *s == '\t') s++;

        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {
            if (has_section && current_url[0]) {
                if (valid_repo_name(current_name) && valid_repo_url(current_url)) {
                    snprintf(repos[count].name, sizeof(repos[count].name), "%s", current_name);
                    snprintf(repos[count].url, sizeof(repos[count].url), "%s", current_url);
                    repos[count].priority = current_priority;
                    count++;
                }
            }
            has_section = 1;
            current_url[0] = '\0';
            current_priority = 0;

            char *end = strchr(s, ']');
            if (end) {
                size_t nlen = (size_t)(end - s - 1);
                if (nlen >= sizeof(current_name)) nlen = sizeof(current_name) - 1;
                memcpy(current_name, s + 1, nlen);
                current_name[nlen] = '\0';
            } else {
                snprintf(current_name, sizeof(current_name), "unnamed");
            }
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        size_t klen = strlen(key);
        while (klen > 0 && (key[klen-1] == ' ' || key[klen-1] == '\t')) key[--klen] = '\0';

        /* trim value + strip quotes */
        while (*val == ' ' || *val == '\t') val++;
        size_t vlen = strlen(val);
        while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';
        {
            char *hash = strchr(val, '#');
            char *semi = strchr(val, ';');
            char *comment = NULL;
            if (hash && semi) comment = hash < semi ? hash : semi;
            else comment = hash ? hash : semi;
            if (comment) {
                vlen = (size_t)(comment - val);
                val[vlen] = '\0';
                while (vlen > 0 && (val[vlen-1] == ' ' || val[vlen-1] == '\t')) val[--vlen] = '\0';
            }
        }
        if (vlen >= 2 &&
            ((val[0] == '"' && val[vlen-1] == '"') ||
             (val[0] == '\'' && val[vlen-1] == '\''))) {
            val++;
            vlen -= 2;
            val[vlen] = '\0';
        }

        if (strcmp(key, "url") == 0) {
            snprintf(current_url, sizeof(current_url), "%s", val);
        } else if (strcmp(key, "priority") == 0) {
            unsigned v = 0;
            int neg = 1;
            const char *p = val;
            if (*p == '-') { neg = -1; p++; }
            if (!isdigit((unsigned char) *p)) continue;
            int valid = 1;
            while (*p >= '0' && *p <= '9') {
                unsigned digit = (unsigned) (*p - '0');
                unsigned limit = neg < 0 ? (unsigned) INT_MAX + 1U :
                                           (unsigned) INT_MAX;
                if (v > (limit - digit) / 10U) {
                    valid = 0;
                    break;
                }
                v = v * 10U + digit;
                p++;
            }
            if (*p != '\0') valid = 0;
            if (valid)
                current_priority = neg < 0 && v == (unsigned) INT_MAX + 1U ?
                                       INT_MIN :
                                       (int) v * neg;
        }
    }

    if (has_section && current_url[0] && count < max &&
        valid_repo_name(current_name) && valid_repo_url(current_url)) {
        snprintf(repos[count].name, sizeof(repos[count].name), "%s", current_name);
        snprintf(repos[count].url, sizeof(repos[count].url), "%s", current_url);
        repos[count].priority = current_priority;
        count++;
    }

    free(txt);

    for (int i = 1; i < count; i++) {
        RepoConfig key = repos[i];
        int j = i - 1;
        while (j >= 0 && repos[j].priority < key.priority) {
            repos[j + 1] = repos[j];
            j--;
        }
        repos[j + 1] = key;
    }

    return count;
}

void write_repos(const RepoConfig *repos, int count) {
    FILE *f = fopen(REPO_SOURCES_PATH, "w");
    if (!f) dief("cannot write %s", REPO_SOURCES_PATH);

    for (int i = 0; i < count; i++) {
        fprintf(f, "[%s]\n", repos[i].name);
        fprintf(f, "url=%s\n", repos[i].url);
        fprintf(f, "priority=%d\n", repos[i].priority);
        if (i < count - 1) fputc('\n', f);
    }
    if (fclose(f) != 0) dief("cannot save %s", REPO_SOURCES_PATH);
    if (chmod(REPO_SOURCES_PATH, 0600) != 0)
        dief("cannot protect %s", REPO_SOURCES_PATH);
}

void add_repo(const char *name, const char *url, int priority) {
    if (!valid_repo_name(name)) dief("invalid repository name");
    if (!valid_repo_url(url)) dief("invalid repository URL");
    RepoConfig repos[MAX_REPOS];
    int count = read_repos(repos, MAX_REPOS);

    for (int i = 0; i < count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            snprintf(repos[i].url, sizeof(repos[i].url), "%s", url);
            repos[i].priority = priority;
            write_repos(repos, count);
            log_done("repository '%s' updated", name);
            return;
        }
    }

    if (count >= MAX_REPOS) dief("too many repositories (max %d)", MAX_REPOS);

    snprintf(repos[count].name, sizeof(repos[count].name), "%s", name);
    snprintf(repos[count].url, sizeof(repos[count].url), "%s", url);
    repos[count].priority = priority;
    count++;

    write_repos(repos, count);
    log_done("repository '%s' added", name);
}

void remove_repo(const char *name) {
    if (!valid_repo_name(name)) dief("invalid repository name");
    RepoConfig repos[MAX_REPOS];
    int count = read_repos(repos, MAX_REPOS);
    int found = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(repos[i].name, name) == 0) {
            found = 1;
            for (int j = i; j < count - 1; j++)
                repos[j] = repos[j + 1];
            count--;
            break;
        }
    }

    if (!found) dief("repository '%s' not found", name);

    write_repos(repos, count);
    log_done("repository '%s' removed", name);
}

long disk_available(const char *path) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return -1;
    return (long)(vfs.f_bavail * vfs.f_frsize);
}

pid_t spinner_start(void) {
    pid_t pid = fork();
    if (pid == 0) {
        const char frames[] = "|/-\\";
        int i = 0;
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = SIG_DFL;
        sigaction(SIGTERM, &sa, NULL);
        setbuf(stderr, NULL);
        while (1) {
            if (write(STDERR_FILENO, "\r", 1) == -1) break;
            if (write(STDERR_FILENO, &frames[i], 1) == -1) break;
            i = (i + 1) & 3;
            struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 };
            nanosleep(&ts, NULL);
        }
        _exit(0);
    }
    return pid;
}

void spinner_stop(pid_t pid) {
    if (pid <= 0) return;
    int status;
    kill(pid, SIGTERM);
    waitpid(pid, &status, 0);
    if (write(STDERR_FILENO, "\r \r", 3) == -1) return;
}
