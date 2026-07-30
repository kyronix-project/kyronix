#include "common.h"
#include <sys/stat.h>

static int max_depth = INT_MAX;
static bool show_hidden = false;
static bool show_files = true;
static bool show_dirs = true;
static bool show_symlinks = true;
static bool show_only_dirs = false;
static bool show_size = false;
static bool show_perms = false;
static bool show_type = false;
static FILE *out_file = NULL;

#define out_printf(...) fprintf(out_file ? out_file : stdout, __VA_ARGS__)

static void usage(void) {
    fprintf(stderr, "usage: tree [-adfFhlps] [-L LEVEL] [-o FILE] [PATH...]\n");
    fprintf(stderr, "  -a          show hidden files\n");
    fprintf(stderr, "  -d          list directories only\n");
    fprintf(stderr, "  -f          show full path prefix for each file\n");
    fprintf(stderr, "  -F          append / @ * = | indicator\n");
    fprintf(stderr, "  -h          show help\n");
    fprintf(stderr, "  -l          follow symlinks\n");
    fprintf(stderr, "  -L LEVEL    max display depth\n");
    fprintf(stderr, "  -o FILE     output to file\n");
    fprintf(stderr, "  -p          show permissions\n");
    fprintf(stderr, "  -s          show file size\n");
}

static int cmp_name(const void *a, const void *b) {
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

static bool is_hidden(const char *name) {
    return name[0] == '.';
}

static int should_show(const char *name, struct stat *st, bool follow) {
    if (!show_hidden && is_hidden(name)) return 0;
    if (S_ISDIR(st->st_mode)) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
        return show_dirs;
    }
    if (S_ISLNK(st->st_mode)) {
        if (follow) {
            struct stat target;
            if (stat(name, &target) == 0) {
                if (S_ISDIR(target.st_mode)) return show_dirs;
            }
        }
        return show_symlinks;
    }
    return show_files && !show_only_dirs;
}

static char type_indicator(mode_t m) {
    if (S_ISDIR(m)) return '/';
    if (S_ISLNK(m)) return '@';
    if (S_ISSOCK(m)) return '=';
    if (S_ISFIFO(m)) return '|';
    if (m & 0111) return '*';
    return 0;
}

static void mode_string(mode_t m, char out[12]) {
    out[0] = S_ISDIR(m)  ? 'd' :
             S_ISLNK(m)  ? 'l' :
             S_ISCHR(m)  ? 'c' :
             S_ISBLK(m)  ? 'b' :
             S_ISFIFO(m) ? 'p' :
             S_ISSOCK(m) ? 's' :
                           '-';
    const char *c = "rwx";
    for (int i = 0; i < 9; i++) out[i + 1] = (m & (1 << (8 - i))) ? c[i % 3] : '-';
    out[10] = 0;
}

static char **list_dir(const char *path, int *out_n, bool follow, int *rc) {
    DIR *d = opendir(path);
    if (!d) {
        kx_warn(path);
        *rc = 1;
        *out_n = 0;
        return NULL;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *entry;
    char full[PATH_MAX];

    while ((entry = readdir(d)) != NULL) {
        int nlen = snprintf(full, sizeof(full), "%s/%s", path, entry->d_name);
        if (nlen < 0 || (size_t)nlen >= sizeof(full)) {
            errno = ENAMETOOLONG;
            kx_warn(path);
            *rc = 1;
            continue;
        }
        struct stat st;
        int stat_ok = follow ? stat(full, &st) : lstat(full, &st);
        if (stat_ok < 0) {
            if (errno == ENOENT) continue;
            kx_warn(full);
            *rc = 1;
            continue;
        }
        if (!should_show(entry->d_name, &st, follow)) continue;

        if (n >= cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, (size_t)cap * sizeof(char *));
        }
        names[n++] = strdup(entry->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(char *), cmp_name);
    *out_n = n;
    return names;
}

static void print_entry(const char *display, struct stat *st, const char *prefix,
                        bool last) {
    if (show_perms || show_size) {
        out_printf("%s", prefix);
        out_printf("%s", last ? "\\-- " : "+-- ");
        out_printf("[");
        if (show_perms) {
            char m[12];
            mode_string(st->st_mode, m);
            out_printf("%s", m);
            if (show_size) out_printf(" ");
        }
        if (show_size)
            out_printf("%8lld", (long long)st->st_size);
        out_printf("]  ");
        out_printf("%s", display);
    } else {
        out_printf("%s%s%s", prefix, last ? "\\-- " : "+-- ", display);
    }

    if (show_type && S_ISREG(st->st_mode) && (st->st_mode & 0111))
        out_printf("*");

    out_printf("\n");
}

static void print_tree(const char *path, int depth, const char *prefix, bool last, bool full_path,
                       bool follow, int *rc) {
    if (depth > max_depth) return;

    struct stat st;
    int stat_ok = follow ? stat(path, &st) : lstat(path, &st);
    if (stat_ok < 0) {
        kx_warn(path);
        *rc = 1;
        return;
    }

    const char *display = full_path ? path : kx_base(path);
    if (depth == 0) {
        out_printf("%s", display);
        if (show_type) {
            char ti = type_indicator(st.st_mode);
            if (ti) out_printf("%c", ti);
        }
        out_printf("\n");
    } else {
        print_entry(display, &st, prefix, last);
    }

    if (!S_ISDIR(st.st_mode)) return;
    if (S_ISLNK(st.st_mode) && follow) {
        struct stat target;
        if (stat(path, &target) == 0 && !S_ISDIR(target.st_mode)) return;
    }

    int n;
    char **entries = list_dir(path, &n, follow, rc);
    if (!entries) return;

    char new_prefix[PATH_MAX];
    const char *branch = last ? "    " : "|   ";
    if (prefix[0] == '\0')
        snprintf(new_prefix, sizeof(new_prefix), "%s", branch);
    else
        snprintf(new_prefix, sizeof(new_prefix), "%s%s", prefix, branch);

    for (int i = 0; i < n; i++) {
        char child[PATH_MAX];
        int nlen = snprintf(child, sizeof(child), "%s/%s", path, entries[i]);
        if (nlen < 0 || (size_t)nlen >= sizeof(child)) {
            errno = ENAMETOOLONG;
            kx_warn(path);
            *rc = 1;
            free(entries[i]);
            continue;
        }
        print_tree(child, depth + 1, new_prefix, i == n - 1, full_path, follow, rc);
        free(entries[i]);
    }
    free(entries);
}

int main(int argc, char **argv) {
    kx_prog = "tree";
    bool full_path = false;
    bool follow = false;
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        }
        if ((strcmp(arg, "-L") == 0) && first + 1 < argc) {
            max_depth = atoi(argv[++first]);
            if (max_depth < 0) max_depth = INT_MAX;
            continue;
        }
        if ((strcmp(arg, "-o") == 0) && first + 1 < argc) {
            first++;
            out_file = fopen(argv[first], "w");
            if (!out_file) {
                kx_warn(argv[first]);
                return 1;
            }
            continue;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'a': show_hidden = true; break;
            case 'd': show_dirs = true; show_files = false; show_only_dirs = true; break;
            case 'f': full_path = true; break;
            case 'F': show_type = true; break;
            case 'h': usage(); return 0;
            case 'l': follow = true; break;
            case 'p': show_perms = true; break;
            case 's': show_size = true; break;
            default: usage(); return 1;
            }
        }
    }

    int rc = 0;
    if (first == argc) {
        print_tree(".", 0, "", true, full_path, follow, &rc);
    } else {
        for (int i = first; i < argc; i++) {
            if (i > first) out_printf("\n");
            print_tree(argv[i], 0, "", true, full_path, follow, &rc);
        }
    }

    if (out_file) fclose(out_file);
    return rc;
}
