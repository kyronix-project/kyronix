#include "common.h"

static void print_name(const char *path, const char *suffix, bool zero) {
    char copy[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(copy)) kx_die("name too long");
    memcpy(copy, path, len + 1);

    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    char *name = strrchr(copy, '/');
    name = name ? name + 1 : copy;
    if (copy[0] == '/' && copy[1] == '\0') name = copy;

    if (suffix && suffix[0]) {
        size_t nlen = strlen(name);
        size_t slen = strlen(suffix);
        if (slen < nlen && memcmp(name + nlen - slen, suffix, slen) == 0)
            name[nlen - slen] = '\0';
    }
    fputs(name, stdout);
    putchar(zero ? '\0' : '\n');
}

int main(int argc, char **argv) {
    kx_prog = "basename";
    bool multiple = false;
    bool zero = false;
    const char *suffix = NULL;
    int i = 1;

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-a") == 0) {
            multiple = true;
        } else if (strcmp(argv[i], "-z") == 0) {
            zero = true;
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            suffix = argv[++i];
            multiple = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            puts("usage: basename [-az] [-s SUFFIX] NAME...");
            return 0;
        } else if (argv[i][0] == '-' && argv[i][1]) {
            kx_die("usage: basename [-az] [-s SUFFIX] NAME...");
        } else {
            break;
        }
    }
    if (i >= argc) kx_die("missing operand");
    if (!multiple && argc - i > 2) kx_die("extra operand");
    if (!multiple && argc - i == 2) suffix = argv[i + 1];

    int end = multiple ? argc : i + 1;
    for (; i < end; i++) print_name(argv[i], suffix, zero);
    return 0;
}
