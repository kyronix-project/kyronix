#include "common.h"

typedef enum { MODE_LINES, MODE_BYTES } tail_mode_t;

typedef struct {
    tail_mode_t mode;
    unsigned long long count;
    bool from_start;
    bool quiet;
    bool verbose;
    bool follow;
} options_t;

static void usage(void) {
    fprintf(stderr, "usage: tail [-fFqv] [-n NUMBER | -c NUMBER] [FILE...]\n");
}

static bool parse_count(const char *s, unsigned long long *value, bool *from_start) {
    *from_start = *s == '+';
    if (*s == '+' || *s == '-') s++;
    char *end;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10);
    if (end == s || *end || errno == ERANGE) return false;
    *value = n;
    return true;
}

static int read_all(FILE *f, unsigned char **data, size_t *length) {
    size_t cap = 0, len = 0;
    unsigned char *buf = NULL;
    for (;;) {
        if (len == cap) {
            size_t next = cap ? cap * 2 : 8192;
            unsigned char *grown = realloc(buf, next);
            if (!grown) {
                free(buf);
                return -1;
            }
            buf = grown;
            cap = next;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) break;
    }
    if (ferror(f)) {
        free(buf);
        return -1;
    }
    *data = buf;
    *length = len;
    return 0;
}

static size_t line_start_from_front(const unsigned char *data, size_t len,
                                    unsigned long long line) {
    if (line <= 1) return 0;
    unsigned long long current = 1;
    for (size_t i = 0; i < len; i++)
        if (data[i] == '\n' && ++current == line) return i + 1;
    return len;
}

static size_t line_start_from_back(const unsigned char *data, size_t len,
                                   unsigned long long count) {
    if (count == 0) return len;
    size_t pos = len;
    if (pos && data[pos - 1] == '\n') pos--;
    unsigned long long seen = 0;
    while (pos > 0) {
        pos--;
        if (data[pos] == '\n' && ++seen == count) return pos + 1;
    }
    return 0;
}

static int show_initial(FILE *f, const options_t *o) {
    unsigned char *data;
    size_t len;
    if (read_all(f, &data, &len) < 0) return 1;
    size_t start;
    if (o->mode == MODE_BYTES) {
        if (o->from_start) {
            unsigned long long offset = o->count ? o->count - 1 : 0;
            start = offset > len ? len : (size_t) offset;
        } else {
            start = o->count >= len ? 0 : len - (size_t) o->count;
        }
    } else if (o->from_start) {
        start = line_start_from_front(data, len, o->count);
    } else {
        start = line_start_from_back(data, len, o->count);
    }
    int rc = fwrite(data + start, 1, len - start, stdout) == len - start ? 0 : 1;
    free(data);
    return rc;
}

static void header(const char *name, bool *first) {
    if (!*first) putchar('\n');
    printf("==> %s <==\n", name);
    *first = false;
}

int main(int argc, char **argv) {
    kx_prog = "tail";
    options_t o = { .mode = MODE_LINES, .count = 10 };
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        if (a[1] >= '0' && a[1] <= '9') {
            if (!parse_count(a + 1, &o.count, &o.from_start)) kx_die("invalid count");
            o.mode = MODE_LINES;
            continue;
        }
        if (strncmp(a, "--lines=", 8) == 0 || strncmp(a, "--bytes=", 8) == 0) {
            o.mode = a[2] == 'l' ? MODE_LINES : MODE_BYTES;
            if (!parse_count(a + 8, &o.count, &o.from_start)) kx_die("invalid count");
            continue;
        }
        if (strcmp(a, "--follow") == 0) {
            o.follow = true;
            continue;
        }
        if (strcmp(a, "--quiet") == 0 || strcmp(a, "--silent") == 0) {
            o.quiet = true;
            continue;
        }
        if (strcmp(a, "--verbose") == 0) {
            o.verbose = true;
            continue;
        }
        if (a[1] == 'n' || a[1] == 'c') {
            o.mode = a[1] == 'n' ? MODE_LINES : MODE_BYTES;
            const char *value = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!value || !parse_count(value, &o.count, &o.from_start)) kx_die("invalid count");
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'f' || *p == 'F') o.follow = true;
            else if (*p == 'q') o.quiet = true;
            else if (*p == 'v') o.verbose = true;
            else {
                usage();
                return 1;
            }
        }
    }
    if (i == argc) argv[argc++] = NULL;
    if (i < 0 || i > argc) return 1;
    size_t nfiles = (size_t) (argc - i);
    bool show_headers = o.verbose || (nfiles > 1 && !o.quiet);
    bool first_header = true;
    FILE **streams = calloc((size_t) nfiles, sizeof(*streams));
    if (!streams) return 1;

    int rc = 0;
    for (size_t n = 0; n < nfiles; n++) {
        const char *name = argv[i + n];
        FILE *f = !name || strcmp(name, "-") == 0 ? stdin : fopen(name, "rb");
        streams[n] = f;
        if (!f) {
            kx_warn(name);
            rc = 1;
            continue;
        }
        if (show_headers) header(name ? name : "standard input", &first_header);
        if (show_initial(f, &o) != 0) {
            kx_warn(name ? name : "standard input");
            rc = 1;
        }
    }

    while (o.follow) {
        bool wrote = false;
        for (size_t n = 0; n < nfiles; n++) {
            FILE *f = streams[n];
            if (!f || f == stdin) continue;
            clearerr(f);
            int c;
            bool this_file = false;
            while ((c = fgetc(f)) != EOF) {
                if (show_headers && !this_file) {
                    header(argv[i + n], &first_header);
                    this_file = true;
                }
                putchar(c);
                wrote = true;
            }
        }
        if (wrote) fflush(stdout);
        sleep(1);
    }
    for (size_t n = 0; n < nfiles; n++)
        if (streams[n] && streams[n] != stdin) fclose(streams[n]);
    free(streams);
    return rc || ferror(stdout) ? 1 : 0;
}
