#include "common.h"
#include <sys/wait.h>

typedef struct {
    bool force;
    bool interactive;
    bool no_clobber;
    bool verbose;
} mv_options_t;

static bool confirm_replace(const char *path) {
    fprintf(stderr, "mv: overwrite '%s'? ", path);
    fflush(stderr);
    int answer = getchar();
    while (answer != '\n' && answer != EOF) {
        int next = getchar();
        if (next == '\n' || next == EOF) break;
    }
    return answer == 'y' || answer == 'Y';
}

static int append_basename(char *out, size_t size, const char *directory, const char *source) {
    char source_copy[PATH_MAX];
    size_t source_len = strlen(source);
    if (source_len >= sizeof(source_copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(source_copy, source, source_len + 1);
    while (source_len > 1 && source_copy[source_len - 1] == '/')
        source_copy[--source_len] = '\0';
    int n = snprintf(out, size, "%s%s%s", directory,
                     directory[0] && directory[strlen(directory) - 1] == '/' ? "" : "/",
                     kx_base(source_copy));
    if (n < 0 || (size_t) n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int remove_tree(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);

    DIR *dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t) n >= sizeof(child) || remove_tree(child) < 0) {
            rc = -1;
            break;
        }
    }
    closedir(dir);
    if (rc == 0) rc = rmdir(path);
    return rc;
}

static int copy_for_cross_device(const char *source, const char *target) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("cp", "cp", "-a", "--", source, target, NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        return -1;
    }
    return remove_tree(source);
}

static int move_one(const char *source, const char *target, const mv_options_t *o) {
    struct stat source_st;
    if (lstat(source, &source_st) < 0) {
        kx_warn(source);
        return -1;
    }
    struct stat target_st;
    bool exists = lstat(target, &target_st) == 0;
    if (exists && source_st.st_dev == target_st.st_dev && source_st.st_ino == target_st.st_ino) {
        fprintf(stderr, "mv: '%s' and '%s' are the same file\n", source, target);
        return -1;
    }
    if (exists && o->no_clobber) return 0;
    if (exists && o->interactive && !confirm_replace(target)) return 0;

    if (rename(source, target) < 0) {
        if (errno != EXDEV || copy_for_cross_device(source, target) < 0) {
            kx_warn(source);
            return -1;
        }
    }
    if (o->verbose) printf("renamed '%s' -> '%s'\n", source, target);
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "mv";
    mv_options_t o = {0};
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: mv [-finv] SOURCE... DEST");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'f': o.force = true; o.interactive = o.no_clobber = false; break;
            case 'i': o.interactive = true; o.force = o.no_clobber = false; break;
            case 'n': o.no_clobber = true; o.force = o.interactive = false; break;
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
            if (append_basename(target, sizeof(target), destination, argv[i]) < 0) {
                kx_warn(destination);
                rc = 1;
                continue;
            }
            resolved = target;
        }
        if (move_one(argv[i], resolved, &o) < 0) rc = 1;
    }
    return rc;
}
