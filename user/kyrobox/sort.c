#include "common.h"

typedef struct {
    bool numeric;
    bool reverse;
    bool unique;
    int key_start;
    int key_end;
    char sep;
} opts_t;

static opts_t o;

static const char *field_ptr(const char *line, const char *end, int field) {
    const char *p = line;
    if (!o.sep) {
        while (p < end && isspace((unsigned char) *p)) p++;
    }
    for (int cur = 1; cur < field && p < end; cur++) {
        if (o.sep) {
            while (p < end && *p != o.sep) p++;
            if (p < end) p++;
        } else {
            while (p < end && !isspace((unsigned char) *p)) p++;
            while (p < end && isspace((unsigned char) *p)) p++;
        }
    }
    return p;
}

static const char *field_end_ptr(const char *p, const char *end) {
    if (o.sep) {
        while (p < end && *p != o.sep) p++;
    } else {
        while (p < end && !isspace((unsigned char) *p)) p++;
    }
    return p;
}

static void get_key(const char *line, const char **ks, const char **ke) {
    size_t len = strlen(line);
    const char *end = line + len;
    if (len && end[-1] == '\n') end--;
    if (o.key_start <= 0) {
        *ks = line;
        *ke = end;
        return;
    }
    const char *start = field_ptr(line, end, o.key_start);
    const char *stop;
    if (o.key_end > 0) {
        const char *fs = field_ptr(line, end, o.key_end);
        stop = field_end_ptr(fs, end);
    } else {
        stop = end;
    }
    if (stop < start) stop = start;
    *ks = start;
    *ke = stop;
}

static double parse_num(const char *s, size_t len) {
    char *buf = malloc(len + 1);
    memcpy(buf, s, len);
    buf[len] = 0;
    double v = strtod(buf, NULL);
    free(buf);
    return v;
}

static int compare_keys(const char *a, const char *b) {
    const char *as, *ae, *bs, *be;
    get_key(a, &as, &ae);
    get_key(b, &bs, &be);
    if (o.numeric) {
        double av = parse_num(as, (size_t) (ae - as));
        double bv = parse_num(bs, (size_t) (be - bs));
        return (av > bv) - (av < bv);
    }
    size_t alen = (size_t) (ae - as), blen = (size_t) (be - bs);
    size_t minlen = alen < blen ? alen : blen;
    int r = memcmp(as, bs, minlen);
    if (r != 0) return r;
    return (int) alen - (int) blen;
}

static int cmp_lines(const void *a, const void *b) {
    const char *const *sa = a, *const *sb = b;
    int r = compare_keys(*sa, *sb);
    return o.reverse ? -r : r;
}

static void parse_key(const char *val) {
    char *end;
    long start = strtol(val, &end, 10);
    if (end == val || start <= 0) kx_die("invalid -k key");
    o.key_start = (int) start;
    o.key_end = 0;
    if (*end == ',') {
        long stop = strtol(end + 1, NULL, 10);
        if (stop > 0) o.key_end = (int) stop;
    }
}

int main(int argc, char **argv) {
    kx_prog = "sort";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strncmp(a, "-k", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: sort [-nru] [-k KEYDEF] [-t SEP] [FILE...]");
            parse_key(val);
            continue;
        }
        if (strncmp(a, "-t", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val || !val[0]) kx_die("usage: sort [-nru] [-k KEYDEF] [-t SEP] [FILE...]");
            o.sep = val[0];
            continue;
        }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'n':
                o.numeric = true;
                break;
            case 'r':
                o.reverse = true;
                break;
            case 'u':
                o.unique = true;
                break;
            default:
                kx_die("usage: sort [-nru] [-k KEYDEF] [-t SEP] [FILE...]");
            }
        }
    }
    if (i == argc) argv[argc++] = NULL;
    char **lines = NULL;
    size_t nlines = 0, caplines = 0;
    int rc = 0;
    for (int a = i; a < argc; a++) {
        FILE *f = argv[a] ? fopen(argv[a], "r") : stdin;
        if (!f) {
            kx_warn(argv[a]);
            rc = 1;
            continue;
        }
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            if (nlines == caplines) {
                caplines = caplines ? caplines * 2 : 64;
                lines = realloc(lines, caplines * sizeof(*lines));
            }
            lines[nlines++] = strdup(line);
        }
        free(line);
        if (argv[a]) fclose(f);
    }
    qsort(lines, nlines, sizeof(*lines), cmp_lines);
    for (size_t n = 0; n < nlines; n++) {
        if (o.unique && n > 0 && compare_keys(lines[n - 1], lines[n]) == 0) {
            free(lines[n]);
            continue;
        }
        fputs(lines[n], stdout);
        free(lines[n]);
    }
    free(lines);
    return rc;
}
