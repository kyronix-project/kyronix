#include "common.h"

typedef struct {
    bool all;
    bool summarize;
    bool human;
    bool total;
    bool apparent;
    unsigned long long unit;
    int max_depth;
} du_options_t;

static void print_size(unsigned long long bytes, const char *path, const du_options_t *o) {
    if (o->human) {
        static const char suffix[] = "BKMGTPE";
        double value = (double) bytes;
        int index = 0;
        while (value >= 1024.0 && index < 6) {
            value /= 1024.0;
            index++;
        }
        if (index == 0)
            printf("%llu%c\t%s\n", bytes, suffix[index], path);
        else
            printf(value < 10.0 ? "%.1f%c\t%s\n" : "%.0f%c\t%s\n", value, suffix[index], path);
    } else {
        unsigned long long units = (bytes + o->unit - 1) / o->unit;
        printf("%llu\t%s\n", units, path);
    }
}

static unsigned long long disk_bytes(const struct stat *st, const du_options_t *o) {
    if (o->apparent || st->st_blocks == 0) return (unsigned long long) st->st_size;
    return (unsigned long long) st->st_blocks * 512ULL;
}

static unsigned long long walk_path(const char *path, int depth, const du_options_t *o, int *rc) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        *rc = 1;
        return 0;
    }
    unsigned long long bytes = disk_bytes(&st, o);
    if (!S_ISDIR(st.st_mode)) {
        if (o->all && depth <= o->max_depth) print_size(bytes, path, o);
        return bytes;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        kx_warn(path);
        *rc = 1;
        return bytes;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        if (n < 0 || (size_t) n >= sizeof(child)) {
            errno = ENAMETOOLONG;
            kx_warn(path);
            *rc = 1;
            continue;
        }
        bytes += walk_path(child, depth + 1, o, rc);
    }
    closedir(dir);
    if ((!o->summarize || depth == 0) && depth <= o->max_depth) print_size(bytes, path, o);
    return bytes;
}

int main(int argc, char **argv) {
    kx_prog = "du";
    du_options_t o = { .unit = 1024, .max_depth = INT_MAX };
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: du [-abchkmst] [-d DEPTH] [FILE...]");
            return 0;
        }
        if ((strcmp(arg, "-d") == 0 || strcmp(arg, "--max-depth") == 0) &&
            first + 1 < argc) {
            o.max_depth = atoi(argv[++first]);
            if (o.max_depth < 0) kx_die("invalid depth");
            continue;
        }
        if (strncmp(arg, "--max-depth=", 12) == 0) {
            o.max_depth = atoi(arg + 12);
            if (o.max_depth < 0) kx_die("invalid depth");
            continue;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'a': o.all = true; break;
            case 'b': o.apparent = true; o.unit = 1; break;
            case 'c': o.total = true; break;
            case 'h': o.human = true; break;
            case 'k': o.human = false; o.unit = 1024; break;
            case 'm': o.human = false; o.unit = 1024 * 1024; break;
            case 's': o.summarize = true; break;
            default: kx_die("invalid option");
            }
        }
    }

    int paths = argc - first;
    int count = paths ? paths : 1;
    unsigned long long total = 0;
    int rc = 0;
    for (int i = 0; i < count; i++) {
        const char *path = paths ? argv[first + i] : ".";
        total += walk_path(path, 0, &o, &rc);
    }
    if (o.total) print_size(total, "total", &o);
    return rc;
}
