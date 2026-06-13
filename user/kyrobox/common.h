#ifndef KYROBOX_COMMON_H
#define KYROBOX_COMMON_H

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static const char* kx_prog;

static inline const char* kx_base(const char* s)
{
    const char* p = strrchr(s, '/');
    return p ? p + 1 : s;
}

static inline void kx_warn(const char* path)
{
    fprintf(stderr, "%s: %s: %s\n", kx_prog, path, strerror(errno));
}

static inline void kx_die(const char* msg)
{
    fprintf(stderr, "%s: %s\n", kx_prog, msg);
    exit(1);
}

static inline int kx_copy_fd(int in, int out)
{
    char buf[8192];
    for (;;) {
        ssize_t n = read(in, buf, sizeof(buf));
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        char* p = buf;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t) n);
            if (w < 0)
                return -1;
            p += w;
            n -= w;
        }
    }
}

static inline int kx_mkdir_p(const char* path, mode_t mode)
{
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(tmp, path);
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static inline int kx_copy_file(const char* src, const char* dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out < 0) {
        close(in);
        return -1;
    }
    int r = kx_copy_fd(in, out);
    close(in);
    close(out);
    return r;
}

#endif
