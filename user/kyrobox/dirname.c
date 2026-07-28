#include "common.h"

static char *directory_name(const char *path) {
    if (!*path) return strdup(".");
    char *copy = strdup(path);
    if (!copy) return NULL;
    size_t len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';

    char *slash = strrchr(copy, '/');
    if (!slash) {
        strcpy(copy, ".");
        return copy;
    }
    while (slash > copy && slash[-1] == '/') slash--;
    if (slash == copy) {
        copy[1] = '\0';
        return copy;
    }
    *slash = '\0';
    len = strlen(copy);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    return copy;
}

int main(int argc, char **argv) {
    kx_prog = "dirname";
    bool zero = false;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-z") == 0 || strcmp(argv[i], "--zero") == 0) {
            zero = true;
            continue;
        }
        if (argv[i][0] == '-' && argv[i][1]) kx_die("usage: dirname [-z] NAME...");
        break;
    }
    if (i == argc) kx_die("missing operand");
    int rc = 0;
    for (; i < argc; i++) {
        char *dir = directory_name(argv[i]);
        if (!dir) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        fputs(dir, stdout);
        putchar(zero ? '\0' : '\n');
        free(dir);
    }
    return rc || ferror(stdout) ? 1 : 0;
}
