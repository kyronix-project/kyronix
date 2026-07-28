#define _GNU_SOURCE
#include "common.h"

extern char **environ;

static bool assignment(const char *value) {
    const char *eq = strchr(value, '=');
    if (!eq || eq == value) return false;
    for (const char *p = value; p < eq; p++)
        if (!(isalnum((unsigned char) *p) || *p == '_') || (p == value && isdigit(*p)))
            return false;
    return true;
}

int main(int argc, char **argv) {
    kx_prog = "env";
    bool empty = false;
    bool zero = false;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "-") == 0) {
            empty = true;
        } else if (strcmp(argv[i], "-0") == 0) {
            zero = true;
        } else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            if (unsetenv(argv[++i]) < 0) {
                kx_warn(argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "-C") == 0 && i + 1 < argc) {
            if (chdir(argv[++i]) < 0) {
                kx_warn(argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("usage: env [-i0] [-u NAME] [-C DIR] [NAME=VALUE]... [COMMAND [ARG...]]");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            kx_die("invalid option");
        } else {
            break;
        }
    }
    if (empty && clearenv() < 0) {
        kx_warn("clearenv");
        return 1;
    }
    while (i < argc && assignment(argv[i])) {
        char *eq = strchr(argv[i], '=');
        *eq = '\0';
        int result = setenv(argv[i], eq + 1, 1);
        *eq = '=';
        if (result < 0) {
            kx_warn(argv[i]);
            return 1;
        }
        i++;
    }
    if (i == argc) {
        for (char **entry = environ; entry && *entry; entry++) {
            fputs(*entry, stdout);
            putchar(zero ? '\0' : '\n');
        }
        return 0;
    }
    execvp(argv[i], argv + i);
    int status = errno == ENOENT ? 127 : 126;
    kx_warn(argv[i]);
    return status;
}
