#include "common.h"
#include <regex.h>

typedef struct {
    bool icase;
    bool invert;
    bool ere;
    bool count;
    bool list;
    bool number;
    bool only;
    bool recursive;
} opts_t;

static opts_t o;
static const char *pattern;
static regex_t re;
static bool have_re;

static char *strcasestr_own(const char *hay, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return (char *) hay;
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char) p[i]) == tolower((unsigned char) needle[i]))
            i++;
        if (i == nlen) return (char *) p;
    }
    return NULL;
}

static bool do_match(const char *line, regmatch_t *m) {
    if (have_re) return regexec(&re, line, 1, m, 0) == 0;
    char *hit = o.icase ? strcasestr_own(line, pattern) : strstr(line, pattern);
    if (!hit) return false;
    m->rm_so = hit - line;
    m->rm_eo = m->rm_so + (regoff_t) strlen(pattern);
    return true;
}

static int grep_stream(FILE *f, const char *name, bool show_name) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long lineno = 0;
    long matches = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        lineno++;
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = 0;
        regmatch_t m;
        bool hit = do_match(line, &m);
        if (o.invert) hit = !hit;
        if (!hit) continue;
        matches++;
        if (o.list) break;
        if (o.count) continue;
        if (show_name) printf("%s:", name);
        if (o.number) printf("%ld:", lineno);
        if (o.only && !o.invert) {
            fwrite(line + m.rm_so, 1, (size_t) (m.rm_eo - m.rm_so), stdout);
            fputc('\n', stdout);
        } else {
            puts(line);
        }
    }
    free(line);
    if (o.list) {
        if (matches) puts(name);
    } else if (o.count) {
        if (show_name) printf("%s:", name);
        printf("%ld\n", matches);
    }
    return matches > 0;
}

static int grep_file(const char *path, bool show_name, int *rc) {
    FILE *f = path ? fopen(path, "r") : stdin;
    if (!f) {
        kx_warn(path);
        *rc = 2;
        return 0;
    }
    int found = grep_stream(f, path ? path : "(standard input)", show_name);
    if (path) fclose(f);
    return found;
}

static int grep_path(const char *path, bool show_name, int *rc);

static int grep_dir(const char *path, int *rc) {
    DIR *d = opendir(path);
    if (!d) {
        kx_warn(path);
        *rc = 2;
        return 0;
    }
    int found = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        found |= grep_path(child, true, rc);
    }
    closedir(d);
    return found;
}

static int grep_path(const char *path, bool show_name, int *rc) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        *rc = 2;
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!o.recursive) {
            fprintf(stderr, "%s: %s: Is a directory\n", kx_prog, path);
            *rc = 2;
            return 0;
        }
        return grep_dir(path, rc);
    }
    return grep_file(path, show_name, rc);
}

int main(int argc, char **argv) {
    kx_prog = "grep";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'i':
                o.icase = true;
                break;
            case 'r':
                o.recursive = true;
                break;
            case 'v':
                o.invert = true;
                break;
            case 'E':
                o.ere = true;
                break;
            case 'c':
                o.count = true;
                break;
            case 'l':
                o.list = true;
                break;
            case 'n':
                o.number = true;
                break;
            case 'o':
                o.only = true;
                break;
            default:
                kx_die("usage: grep [-ircvElno] PATTERN [FILE...]");
            }
        }
    }
    if (i >= argc) kx_die("usage: grep [-ircvElno] PATTERN [FILE...]");
    pattern = argv[i++];

    if (o.ere) {
        int flags = REG_EXTENDED | (o.icase ? REG_ICASE : 0);
        if (regcomp(&re, pattern, flags) != 0) kx_die("bad regular expression");
        have_re = true;
    }

    int rc = 1;
    int found = 0;
    if (i == argc) {
        found |= grep_file(NULL, false, &rc);
    } else {
        bool show_name = (argc - i) > 1 || o.recursive;
        for (; i < argc; i++) found |= grep_path(argv[i], show_name, &rc);
    }

    if (have_re) regfree(&re);
    return found ? 0 : rc;
}
