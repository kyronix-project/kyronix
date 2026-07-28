#include "common.h"

static void usage(void) {
    fprintf(stderr, "usage: mkdir [-pv] [-m MODE] DIRECTORY...\n");
}

static mode_t parse_mode(const char *s) {
    char *end;
    errno = 0;
    unsigned long mode = strtoul(s, &end, 8);
    if (end == s || *end || errno == ERANGE || mode > 07777) kx_die("invalid mode");
    return (mode_t) mode;
}

static int make_one(const char *path, mode_t mode, bool verbose, bool allow_existing) {
    if (mkdir(path, mode) == 0) {
        if (verbose) printf("mkdir: created directory '%s'\n", path);
        return 0;
    }
    if (allow_existing && errno == EEXIST) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    kx_warn(path);
    return 1;
}

static int make_parents(const char *path, mode_t final_mode, bool explicit_mode, bool verbose) {
    size_t len = strlen(path);
    if (len == 0 || len >= PATH_MAX) {
        errno = len ? ENAMETOOLONG : ENOENT;
        kx_warn(path);
        return 1;
    }
    char tmp[PATH_MAX];
    memcpy(tmp, path, len + 1);
    while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (make_one(tmp, 0777, verbose, true) != 0) return 1;
        *p = '/';
        while (p[1] == '/') p++;
    }
    if (make_one(tmp, final_mode, verbose, true) != 0) return 1;
    if (explicit_mode && chmod(tmp, final_mode) < 0) {
        kx_warn(tmp);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "mkdir";
    bool parents = false, verbose = false, explicit_mode = false;
    mode_t mode = 0777;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strncmp(a, "--mode=", 7) == 0) {
            mode = parse_mode(a + 7);
            explicit_mode = true;
            continue;
        }
        if (strcmp(a, "--parents") == 0) {
            parents = true;
            continue;
        }
        if (strcmp(a, "--verbose") == 0) {
            verbose = true;
            continue;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *o = a + 1; *o; o++) {
            if (*o == 'p') parents = true;
            else if (*o == 'v') verbose = true;
            else if (*o == 'm') {
                const char *value = o[1] ? o + 1 : (++i < argc ? argv[i] : NULL);
                if (!value) {
                    usage();
                    return 1;
                }
                mode = parse_mode(value);
                explicit_mode = true;
                break;
            } else {
                usage();
                return 1;
            }
        }
    }
    if (i == argc) {
        usage();
        return 1;
    }

    int rc = 0;
    for (; i < argc; i++) {
        if (parents) rc |= make_parents(argv[i], mode, explicit_mode, verbose);
        else rc |= make_one(argv[i], mode, verbose, false);
    }
    return rc;
}
