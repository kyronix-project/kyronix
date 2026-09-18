#define _POSIX_C_SOURCE 200809L

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
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

const char *home_dir(void) {
    const char *home = getenv("HOME");
    return (home && *home) ? home : "/tmp";
}

char *read_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
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

int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
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
        execvp(argv[0], argv);
        _exit(127);
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) return -1;
    if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
    return -1;
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
                snprintf(repos[count].name, sizeof(repos[count].name), "%s", current_name);
                snprintf(repos[count].url, sizeof(repos[count].url), "%s", current_url);
                repos[count].priority = current_priority;
                count++;
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
        if ((val[0] == '"' && val[vlen-1] == '"') || (val[0] == '\'' && val[vlen-1] == '\'')) {
            val++;
            vlen -= 2;
            val[vlen] = '\0';
        }

        if (strcmp(key, "url") == 0) {
            snprintf(current_url, sizeof(current_url), "%s", val);
        } else if (strcmp(key, "priority") == 0) {
            int v = 0;
            int neg = 1;
            const char *p = val;
            if (*p == '-') { neg = -1; p++; }
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
            current_priority = v * neg;
        }
    }

    if (has_section && current_url[0] && count < max) {
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
    fclose(f);
}

void add_repo(const char *name, const char *url, int priority) {
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
