#define _GNU_SOURCE
#include "common.h"

static int build_template(char *output, size_t size, const char *directory, const char *value) {
    if (strchr(value, '/') || !directory) {
        int n = snprintf(output, size, "%s", value);
        return n >= 0 && (size_t) n < size ? 0 : -1;
    }
    int n = snprintf(output, size, "%s%s%s", directory,
                     directory[0] && directory[strlen(directory) - 1] == '/' ? "" : "/", value);
    return n >= 0 && (size_t) n < size ? 0 : -1;
}

int main(int argc, char **argv) {
    kx_prog = "mktemp";
    bool directory_mode = false;
    bool quiet = false;
    bool dry_run = false;
    const char *tmpdir = NULL;
    const char *suffix = "";
    const char *value = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            if (++i < argc) value = argv[i];
            if (i + 1 < argc) kx_die("extra operand");
            break;
        }
        if (strcmp(arg, "-d") == 0) {
            directory_mode = true;
        } else if (strcmp(arg, "-q") == 0) {
            quiet = true;
        } else if (strcmp(arg, "-u") == 0) {
            dry_run = true;
        } else if ((strcmp(arg, "-p") == 0 || strcmp(arg, "--tmpdir") == 0) &&
                   i + 1 < argc) {
            tmpdir = argv[++i];
        } else if (strncmp(arg, "--tmpdir=", 9) == 0) {
            tmpdir = arg + 9;
        } else if (strcmp(arg, "-t") == 0) {
            tmpdir = getenv("TMPDIR");
            if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
        } else if (strcmp(arg, "--suffix") == 0 && i + 1 < argc) {
            suffix = argv[++i];
        } else if (strncmp(arg, "--suffix=", 9) == 0) {
            suffix = arg + 9;
        } else if (strcmp(arg, "--help") == 0) {
            puts("usage: mktemp [-dqu] [-p DIR|-t] [--suffix SUFFIX] [TEMPLATE]");
            return 0;
        } else if (arg[0] == '-' && arg[1]) {
            kx_die("invalid option");
        } else if (!value) {
            value = arg;
        } else {
            kx_die("extra operand");
        }
    }

    if (!value) {
        if (!tmpdir) {
            tmpdir = getenv("TMPDIR");
            if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
        }
        value = "tmp.XXXXXX";
    }
    if (directory_mode && suffix[0]) kx_die("--suffix is not supported with -d");

    char path[PATH_MAX];
    if (build_template(path, sizeof(path), tmpdir, value) < 0) {
        errno = ENAMETOOLONG;
        if (!quiet) kx_warn(value);
        return 1;
    }
    if (!strstr(path, "XXXXXX")) kx_die("template must contain XXXXXX");

    int rc = 0;
    if (directory_mode) {
        if (!mkdtemp(path)) rc = -1;
        if (rc == 0 && dry_run) rc = rmdir(path);
    } else {
        size_t suffix_len = strlen(suffix);
        if (suffix_len) {
            size_t len = strlen(path);
            if (len + suffix_len >= sizeof(path)) {
                errno = ENAMETOOLONG;
                rc = -1;
            } else {
                memcpy(path + len, suffix, suffix_len + 1);
            }
        }
        int fd = rc == 0 ? mkstemps(path, (int) suffix_len) : -1;
        if (fd < 0)
            rc = -1;
        else {
            close(fd);
            if (dry_run && unlink(path) < 0) rc = -1;
        }
    }
    if (rc < 0) {
        if (!quiet) kx_warn(path);
        return 1;
    }
    puts(path);
    return 0;
}
