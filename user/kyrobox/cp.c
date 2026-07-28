#include "common.h"

typedef struct {
    bool recursive;
    bool force;
    bool interactive;
    bool no_clobber;
    bool preserve;
    bool verbose;
} cp_options_t;

extern char *realpath(const char *restrict path, char *restrict resolved);

static const char *path_basename(const char *path, char *copy, size_t size) {
    size_t len = strlen(path);
    if (len >= size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    memcpy(copy, path, len + 1);
    while (len > 1 && copy[len - 1] == '/') copy[--len] = '\0';
    return kx_base(copy);
}

static bool confirm_replace(const char *path) {
    fprintf(stderr, "cp: overwrite '%s'? ", path);
    fflush(stderr);
    int answer = getchar();
    while (answer != '\n' && answer != EOF) {
        int next = getchar();
        if (next == '\n' || next == EOF) break;
    }
    return answer == 'y' || answer == 'Y';
}

static int destination_path(char *out, size_t size, const char *directory, const char *source) {
    char source_copy[PATH_MAX];
    const char *base = path_basename(source, source_copy, sizeof(source_copy));
    if (!base) return -1;
    int n = snprintf(out, size, "%s%s%s", directory,
                     directory[0] && directory[strlen(directory) - 1] == '/' ? "" : "/", base);
    if (n < 0 || (size_t) n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int preserve_metadata(const char *path, const struct stat *st, bool symlink) {
    int rc = 0;
    if (!symlink && chmod(path, st->st_mode & 07777) < 0) rc = -1;
    int owner_result =
        symlink ? lchown(path, st->st_uid, st->st_gid) : chown(path, st->st_uid, st->st_gid);
    if (owner_result < 0 && errno != EPERM) rc = -1;
    if (!symlink) {
        struct timespec times[2] = { st->st_atim, st->st_mtim };
        if (utimensat(AT_FDCWD, path, times, 0) < 0) rc = -1;
    }
    return rc;
}

static int copy_path(const char *source, const char *target, const cp_options_t *o);

static bool target_is_inside_source(const char *source, const char *target) {
    char source_real[PATH_MAX], target_real[PATH_MAX];
    if (!realpath(source, source_real)) return false;
    if (!realpath(target, target_real)) {
        if (errno != ENOENT) return false;
        char copy[PATH_MAX];
        if (snprintf(copy, sizeof(copy), "%s", target) >= (int) sizeof(copy)) return false;
        char *slash = strrchr(copy, '/');
        const char *base = slash ? slash + 1 : copy;
        const char *parent = ".";
        if (slash) {
            if (slash == copy)
                parent = "/";
            else {
                *slash = '\0';
                parent = copy;
            }
        }
        char parent_real[PATH_MAX];
        if (!realpath(parent, parent_real)) return false;
        if (snprintf(target_real, sizeof(target_real), "%s%s%s", parent_real,
                     parent_real[strlen(parent_real) - 1] == '/' ? "" : "/", base) >=
            (int) sizeof(target_real))
            return false;
    }
    size_t source_len = strlen(source_real);
    return strcmp(source_real, target_real) == 0 ||
           (strncmp(source_real, target_real, source_len) == 0 &&
            target_real[source_len] == '/');
}

static int copy_directory(const char *source, const char *target, const struct stat *st,
                          const cp_options_t *o) {
    if (!o->recursive) {
        fprintf(stderr, "cp: omitting directory '%s'\n", source);
        return -1;
    }

    if (target_is_inside_source(source, target)) {
        fprintf(stderr, "cp: cannot copy '%s' into itself, '%s'\n", source, target);
        return -1;
    }

    struct stat dst_st;
    if (lstat(target, &dst_st) == 0) {
        if (!S_ISDIR(dst_st.st_mode)) {
            errno = ENOTDIR;
            kx_warn(target);
            return -1;
        }
        if (dst_st.st_dev == st->st_dev && dst_st.st_ino == st->st_ino) {
            fprintf(stderr, "cp: '%s' and '%s' are the same directory\n", source, target);
            return -1;
        }
    } else if (errno == ENOENT) {
        if (mkdir(target, st->st_mode & 0777) < 0) {
            kx_warn(target);
            return -1;
        }
    } else {
        kx_warn(target);
        return -1;
    }

    DIR *dir = opendir(source);
    if (!dir) {
        kx_warn(source);
        return -1;
    }
    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child_source[PATH_MAX], child_target[PATH_MAX];
        int a = snprintf(child_source, sizeof(child_source), "%s/%s", source, entry->d_name);
        int b = snprintf(child_target, sizeof(child_target), "%s/%s", target, entry->d_name);
        if (a < 0 || b < 0 || (size_t) a >= sizeof(child_source) ||
            (size_t) b >= sizeof(child_target)) {
            errno = ENAMETOOLONG;
            kx_warn(entry->d_name);
            rc = -1;
            continue;
        }
        if (copy_path(child_source, child_target, o) < 0) rc = -1;
    }
    closedir(dir);
    if (o->preserve && preserve_metadata(target, st, false) < 0) {
        kx_warn(target);
        rc = -1;
    }
    return rc;
}

static int copy_regular(const char *source, const char *target, const struct stat *st,
                        const cp_options_t *o) {
    struct stat dst_st;
    bool exists = lstat(target, &dst_st) == 0;
    if (exists && dst_st.st_dev == st->st_dev && dst_st.st_ino == st->st_ino) {
        fprintf(stderr, "cp: '%s' and '%s' are the same file\n", source, target);
        return -1;
    }
    if (exists && o->no_clobber) return 0;
    if (exists && o->interactive && !confirm_replace(target)) return 0;

    int input = open(source, O_RDONLY);
    if (input < 0) {
        kx_warn(source);
        return -1;
    }
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int output = open(target, flags, st->st_mode & 0777);
    if (output < 0 && exists && o->force) {
        if (unlink(target) == 0) output = open(target, flags, st->st_mode & 0777);
    }
    if (output < 0) {
        kx_warn(target);
        close(input);
        return -1;
    }

    int rc = kx_copy_fd(input, output);
    int saved = errno;
    if (close(input) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    if (close(output) < 0 && rc == 0) {
        rc = -1;
        saved = errno;
    }
    errno = saved;
    if (rc < 0) {
        kx_warn(target);
        return -1;
    }
    if (o->preserve && preserve_metadata(target, st, false) < 0) {
        kx_warn(target);
        return -1;
    }
    return 0;
}

static int copy_symlink(const char *source, const char *target, const struct stat *st,
                        const cp_options_t *o) {
    char link_target[PATH_MAX];
    ssize_t n = readlink(source, link_target, sizeof(link_target) - 1);
    if (n < 0) {
        kx_warn(source);
        return -1;
    }
    link_target[n] = '\0';

    struct stat dst_st;
    if (lstat(target, &dst_st) == 0) {
        if (o->no_clobber) return 0;
        if (o->interactive && !confirm_replace(target)) return 0;
        if (unlink(target) < 0) {
            kx_warn(target);
            return -1;
        }
    } else if (errno != ENOENT) {
        kx_warn(target);
        return -1;
    }
    if (symlink(link_target, target) < 0) {
        kx_warn(target);
        return -1;
    }
    if (o->preserve && preserve_metadata(target, st, true) < 0) {
        kx_warn(target);
        return -1;
    }
    return 0;
}

static int copy_path(const char *source, const char *target, const cp_options_t *o) {
    struct stat st;
    if (lstat(source, &st) < 0) {
        kx_warn(source);
        return -1;
    }

    int rc;
    if (S_ISDIR(st.st_mode))
        rc = copy_directory(source, target, &st, o);
    else if (S_ISLNK(st.st_mode))
        rc = copy_symlink(source, target, &st, o);
    else if (S_ISREG(st.st_mode))
        rc = copy_regular(source, target, &st, o);
    else {
        fprintf(stderr, "cp: unsupported file type: '%s'\n", source);
        return -1;
    }
    if (rc == 0 && o->verbose) printf("'%s' -> '%s'\n", source, target);
    return rc;
}

int main(int argc, char **argv) {
    kx_prog = "cp";
    cp_options_t o = {0};
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: cp [-Rafinpv] SOURCE... DEST");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'R':
            case 'r': o.recursive = true; break;
            case 'a': o.recursive = o.preserve = true; break;
            case 'f': o.force = true; o.interactive = o.no_clobber = false; break;
            case 'i': o.interactive = true; o.force = o.no_clobber = false; break;
            case 'n': o.no_clobber = true; o.force = o.interactive = false; break;
            case 'p': o.preserve = true; break;
            case 'v': o.verbose = true; break;
            default: kx_die("invalid option");
            }
        }
    }
    if (argc - first < 2) kx_die("missing file operand");

    const char *destination = argv[argc - 1];
    int sources = argc - first - 1;
    struct stat dst_st;
    bool destination_is_dir = stat(destination, &dst_st) == 0 && S_ISDIR(dst_st.st_mode);
    if (sources > 1 && !destination_is_dir) kx_die("target is not a directory");

    int rc = 0;
    for (int i = first; i < argc - 1; i++) {
        char target[PATH_MAX];
        const char *resolved = destination;
        if (destination_is_dir) {
            if (destination_path(target, sizeof(target), destination, argv[i]) < 0) {
                kx_warn(destination);
                rc = 1;
                continue;
            }
            resolved = target;
        }
        if (copy_path(argv[i], resolved, &o) < 0) rc = 1;
    }
    return rc;
}
