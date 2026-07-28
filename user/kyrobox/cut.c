#include "common.h"

typedef struct {
    size_t first;
    size_t last;
} range_t;

static range_t ranges[128];
static size_t nranges;
static bool complement;

static void usage(void) {
    fprintf(stderr,
            "usage: cut (-b LIST | -c LIST | -f LIST) [-d DELIM] [-s] [--complement] [FILE...]\n");
}

static int parse_number(const char *s, const char **endp, size_t *value) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(s, &end, 10);
    if (end == s || errno == ERANGE || n == 0 || n > SIZE_MAX) return -1;
    *endp = end;
    *value = (size_t) n;
    return 0;
}

static int parse_list(const char *list) {
    const char *p = list;
    while (*p) {
        if (nranges == sizeof(ranges) / sizeof(ranges[0])) return -1;
        size_t first = 0, last = 0;
        if (*p == '-') {
            p++;
            if (parse_number(p, &p, &last) < 0) return -1;
            first = 1;
        } else {
            if (parse_number(p, &p, &first) < 0) return -1;
            last = first;
            if (*p == '-') {
                p++;
                if (*p && *p != ',') {
                    if (parse_number(p, &p, &last) < 0 || last < first) return -1;
                } else {
                    last = SIZE_MAX;
                }
            }
        }
        ranges[nranges++] = (range_t) { first, last };
        if (*p == ',') p++;
        else if (*p) return -1;
    }
    return nranges ? 0 : -1;
}

static bool selected(size_t position) {
    bool hit = false;
    for (size_t i = 0; i < nranges; i++)
        if (position >= ranges[i].first && position <= ranges[i].last) {
            hit = true;
            break;
        }
    return complement ? !hit : hit;
}

static int cut_stream(FILE *f, int mode, unsigned char delimiter, bool only_delimited,
                      const char *output_delimiter, int record_delimiter) {
    char *record = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getdelim(&record, &cap, record_delimiter, f)) >= 0) {
        bool terminated = len > 0 && (unsigned char) record[len - 1] == (unsigned) record_delimiter;
        size_t data_len = (size_t) len - (terminated ? 1U : 0U);
        if (mode != 'f') {
            for (size_t pos = 1; pos <= data_len; pos++)
                if (selected(pos) && putchar((unsigned char) record[pos - 1]) == EOF) {
                    free(record);
                    return 1;
                }
        } else {
            bool has_delimiter = memchr(record, delimiter, data_len) != NULL;
            if (!has_delimiter) {
                if (!only_delimited && fwrite(record, 1, data_len, stdout) != data_len) {
                    free(record);
                    return 1;
                }
            } else {
                size_t field = 1, start = 0;
                bool wrote = false;
                for (size_t pos = 0; pos <= data_len; pos++) {
                    if (pos != data_len && (unsigned char) record[pos] != delimiter) continue;
                    if (selected(field)) {
                        if (wrote && fputs(output_delimiter, stdout) == EOF) {
                            free(record);
                            return 1;
                        }
                        if (fwrite(record + start, 1, pos - start, stdout) != pos - start) {
                            free(record);
                            return 1;
                        }
                        wrote = true;
                    }
                    field++;
                    start = pos + 1;
                }
            }
        }
        if (terminated && putchar(record_delimiter) == EOF) {
            free(record);
            return 1;
        }
    }
    int rc = ferror(f) || ferror(stdout);
    free(record);
    return rc;
}

int main(int argc, char **argv) {
    kx_prog = "cut";
    int mode = 0, i = 1, record_delimiter = '\n';
    unsigned char delimiter = '\t';
    bool only_delimited = false;
    const char *list = NULL, *output_delimiter = NULL;
    char default_output[2] = { '\t', '\0' };

    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "--complement") == 0) {
            complement = true;
            continue;
        }
        if (strncmp(a, "--output-delimiter=", 19) == 0) {
            output_delimiter = a + 19;
            continue;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        char opt = a[1];
        if (opt == 's' && a[2] == '\0') {
            only_delimited = true;
            continue;
        }
        if (opt == 'z' && a[2] == '\0') {
            record_delimiter = '\0';
            continue;
        }
        if (opt == 'd' || opt == 'b' || opt == 'c' || opt == 'f') {
            const char *value = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!value) {
                usage();
                return 1;
            }
            if (opt == 'd') {
                if (value[0] == '\0' || value[1] != '\0') kx_die("delimiter must be one byte");
                delimiter = (unsigned char) value[0];
                default_output[0] = value[0];
            } else {
                if (mode && mode != opt) kx_die("only one type of list may be specified");
                mode = opt;
                list = value;
            }
            continue;
        }
        usage();
        return 1;
    }

    if (!mode || !list || parse_list(list) < 0 || (mode != 'f' && only_delimited)) {
        usage();
        return 1;
    }
    if (!output_delimiter) output_delimiter = default_output;
    if (i == argc) argv[argc++] = NULL;

    int rc = 0;
    for (; i < argc; i++) {
        FILE *f = !argv[i] || strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "rb");
        if (!f) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        if (cut_stream(f, mode, delimiter, only_delimited, output_delimiter, record_delimiter) != 0)
            rc = 1;
        if (f != stdin) fclose(f);
    }
    return rc;
}
