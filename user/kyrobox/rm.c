#include "common.h"

static int rm_recursive(const char *path, bool force);

static int rm_dir_contents(const char *dirpath, bool force) {
    DIR *d = opendir(dirpath);
    if (!d) {
        if (!force) kx_warn(dirpath);
        return force ? 0 : 1;
    }

    int rc = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", dirpath, ent->d_name);

        if (rm_recursive(child, force) < 0) rc = 1;
    }
    closedir(d);
    return rc;
}

static int rm_recursive(const char *path, bool force) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        if (force) return 0;
        kx_warn(path);
        return -1;
    }

    if (S_ISDIR(st.st_mode)) {
        int rc = rm_dir_contents(path, force);
        if (rmdir(path) < 0) {
            if (!force) kx_warn(path);
            return -1;
        }
        return rc;
    }

    if (unlink(path) < 0) {
        if (force && errno == ENOENT) return 0;
        kx_warn(path);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "rm";
    bool force = false;
    bool recursive = false;
    int first = 1;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') break;
        const char *opt = argv[i] + 1;
        while (*opt) {
            if (*opt == 'f')
                force = true;
            else if (*opt == 'r' || *opt == 'R')
                recursive = true;
            else {
                fprintf(stderr, "rm: unknown option '-%c'\n", *opt);
                return 1;
            }
            opt++;
        }
        first = i + 1;
    }

    if (first == argc && !force) kx_die("missing operand");

    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (recursive) {
            if (rm_recursive(argv[i], force) < 0) rc = 1;
        } else {
            if (unlink(argv[i]) < 0) {
                if (force && errno == ENOENT) continue;
                kx_warn(argv[i]);
                rc = 1;
            }
        }
    }
    return rc;
}
