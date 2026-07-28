#include "common.h"

typedef struct {
    bool symbolic;
    bool force;
    bool interactive;
    bool no_dereference;
    bool no_target_directory;
    bool verbose;
    const char *target_directory;
} options_t;

static void usage(void) {
    fprintf(stderr,
            "usage: ln [-sfinTv] TARGET LINK\n"
            "       ln [-sfinv] TARGET... DIRECTORY\n"
            "       ln [-sfinv] -t DIRECTORY TARGET...\n");
}

static bool is_directory(const char *path, bool follow) {
    struct stat st;
    int rc = follow ? stat(path, &st) : lstat(path, &st);
    return rc == 0 && S_ISDIR(st.st_mode);
}

static int confirm_replace(const char *path) {
    fprintf(stderr, "ln: replace '%s'? ", path);
    fflush(stderr);
    int c = getchar();
    int answer = c == 'y' || c == 'Y';
    while (c != '\n' && c != EOF) c = getchar();
    return answer;
}

static int make_link(const char *target, const char *link_name, const options_t *o) {
    char destination[PATH_MAX];
    const char *dest = link_name;
    if (!o->no_target_directory && is_directory(link_name, !o->no_dereference)) {
        int n = snprintf(destination, sizeof(destination), "%s/%s", link_name, kx_base(target));
        if (n < 0 || (size_t) n >= sizeof(destination)) {
            errno = ENAMETOOLONG;
            kx_warn(link_name);
            return 1;
        }
        dest = destination;
    }

    struct stat st;
    if (lstat(dest, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            errno = EEXIST;
            kx_warn(dest);
            return 1;
        }
        if (o->interactive && !confirm_replace(dest)) return 0;
        if (o->force || o->interactive) {
            if (unlink(dest) < 0) {
                kx_warn(dest);
                return 1;
            }
        }
    }

    int rc = o->symbolic ? symlink(target, dest) : link(target, dest);
    if (rc < 0) {
        kx_warn(dest);
        return 1;
    }
    if (o->verbose) printf("'%s' -> '%s'\n", dest, target);
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "ln";
    options_t o = { 0 };
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--symbolic") == 0) o.symbolic = true;
        else if (strcmp(a, "--force") == 0) {
            o.force = true;
            o.interactive = false;
        } else if (strcmp(a, "--interactive") == 0) {
            o.interactive = true;
            o.force = false;
        } else if (strcmp(a, "--no-dereference") == 0) o.no_dereference = true;
        else if (strcmp(a, "--no-target-directory") == 0) o.no_target_directory = true;
        else if (strcmp(a, "--verbose") == 0) o.verbose = true;
        else if (strncmp(a, "--target-directory=", 19) == 0) o.target_directory = a + 19;
        else {
            for (const char *p = a + 1; *p; p++) {
                if (*p == 's') o.symbolic = true;
                else if (*p == 'f') {
                    o.force = true;
                    o.interactive = false;
                } else if (*p == 'i') {
                    o.interactive = true;
                    o.force = false;
                } else if (*p == 'n') o.no_dereference = true;
                else if (*p == 'T') o.no_target_directory = true;
                else if (*p == 'v') o.verbose = true;
                else if (*p == 't') {
                    const char *value = p[1] ? p + 1 : (++i < argc ? argv[i] : NULL);
                    if (!value) {
                        usage();
                        return 1;
                    }
                    o.target_directory = value;
                    break;
                } else {
                    usage();
                    return 1;
                }
            }
        }
    }

    int operands = argc - i;
    if ((o.target_directory && operands < 1) || (!o.target_directory && operands < 2)) {
        usage();
        return 1;
    }
    const char *directory = o.target_directory;
    if (!directory && operands > 2) directory = argv[argc - 1];
    if (directory && !is_directory(directory, !o.no_dereference)) {
        errno = ENOTDIR;
        kx_warn(directory);
        return 1;
    }

    int last_source = directory && !o.target_directory ? argc - 1 : argc;
    int rc = 0;
    for (; i < last_source; i++) {
        if (directory) {
            char dest[PATH_MAX];
            int n = snprintf(dest, sizeof(dest), "%s/%s", directory, kx_base(argv[i]));
            if (n < 0 || (size_t) n >= sizeof(dest)) {
                errno = ENAMETOOLONG;
                kx_warn(directory);
                rc = 1;
            } else {
                options_t direct = o;
                direct.no_target_directory = true;
                rc |= make_link(argv[i], dest, &direct);
            }
        } else {
            rc |= make_link(argv[i], argv[i + 1], &o);
            break;
        }
    }
    return rc;
}
