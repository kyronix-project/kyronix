#include "common.h"

typedef enum { MODE_LINES, MODE_BYTES } hmode_t;

static hmode_t hmode = MODE_LINES;
static long count = 10;
static bool quiet = false, verbose = false;

static long parse_count(const char *s) {
    if (!*s) kx_die("invalid number of lines/bytes");
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != 0 || errno == ERANGE) kx_die("invalid number of lines/bytes");
    return v;
}

static void print_header(const char *name, bool *first) {
    if (!*first) putchar('\n');
    printf("==> %s <==\n", name);
    *first = false;
}

static void head_lines(FILE *f, long n) {
    char *line = NULL;
    size_t cap = 0;
    long i = 0;
    while (i < n && getline(&line, &cap, f) >= 0) {
        fputs(line, stdout);
        i++;
    }
    free(line);
}

static void head_lines_omit(FILE *f, long omit) {
    char **lines = NULL;
    long n = 0, cap = 0;
    char *buf = NULL;
    size_t bufcap = 0;
    while (getline(&buf, &bufcap, f) >= 0) {
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, (size_t) cap * sizeof(*lines));
        }
        lines[n++] = strdup(buf);
    }
    free(buf);
    long keep = n - omit;
    if (keep < 0) keep = 0;
    for (long i = 0; i < keep; i++) {
        fputs(lines[i], stdout);
        free(lines[i]);
    }
    for (long i = keep; i < n; i++) free(lines[i]);
    free(lines);
}

static void head_bytes(FILE *f, long n) {
    char buf[8192];
    long remaining = n;
    while (remaining > 0) {
        size_t want = remaining < (long) sizeof(buf) ? (size_t) remaining : sizeof(buf);
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        fwrite(buf, 1, got, stdout);
        remaining -= (long) got;
    }
}

static void head_bytes_omit(FILE *f, long omit) {
    char *data = NULL;
    size_t cap = 0, len = 0;
    char buf[8192];
    size_t got;
    while ((got = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (len + got > cap) {
            while (len + got > cap) cap = cap ? cap * 2 : 8192;
            data = realloc(data, cap);
        }
        memcpy(data + len, buf, got);
        len += got;
    }
    long keep = (long) len - omit;
    if (keep < 0) keep = 0;
    fwrite(data, 1, (size_t) keep, stdout);
    free(data);
}

int main(int argc, char **argv) {
    kx_prog = "head";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strncmp(a, "-n", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: head [-n [-]LINES | -c [-]BYTES] [-q|-v] [FILE...]");
            hmode = MODE_LINES;
            count = parse_count(val);
            continue;
        }
        if (strncmp(a, "-c", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: head [-n [-]LINES | -c [-]BYTES] [-q|-v] [FILE...]");
            hmode = MODE_BYTES;
            count = parse_count(val);
            continue;
        }
        if (strcmp(a, "-q") == 0) {
            quiet = true;
            continue;
        }
        if (strcmp(a, "-v") == 0) {
            verbose = true;
            continue;
        }
        kx_die("usage: head [-n [-]LINES | -c [-]BYTES] [-q|-v] [FILE...]");
    }
    if (i == argc) argv[argc++] = NULL;
    bool multi = (argc - i) > 1;
    bool show_headers = verbose || (multi && !quiet);
    bool first = true;
    int rc = 0;
    for (int a = i; a < argc; a++) {
        FILE *f = argv[a] ? fopen(argv[a], "r") : stdin;
        if (!f) {
            kx_warn(argv[a]);
            rc = 1;
            continue;
        }
        if (show_headers) print_header(argv[a] ? argv[a] : "standard input", &first);
        if (hmode == MODE_LINES) {
            if (count >= 0)
                head_lines(f, count);
            else
                head_lines_omit(f, -count);
        } else {
            if (count >= 0)
                head_bytes(f, count);
            else
                head_bytes_omit(f, -count);
        }
        if (argv[a]) fclose(f);
    }
    return rc;
}
