#include "common.h"

typedef struct {
    unsigned long long lines;
    unsigned long long words;
    unsigned long long bytes;
    unsigned long long chars;
    unsigned long long max_line;
} counts_t;

typedef struct {
    bool lines;
    bool words;
    bool bytes;
    bool chars;
    bool max_line;
} wc_options_t;

static int count_stream(FILE *file, counts_t *count) {
    bool in_word = false;
    unsigned long long line_len = 0;
    int c;

    while ((c = fgetc(file)) != EOF) {
        unsigned char byte = (unsigned char) c;
        count->bytes++;
        if ((byte & 0xc0) != 0x80) {
            count->chars++;
            if (c != '\n') line_len++;
        }
        if (c == '\n') {
            count->lines++;
            if (line_len > count->max_line) count->max_line = line_len;
            line_len = 0;
        }
        if (isspace(byte)) {
            in_word = false;
        } else if (!in_word) {
            count->words++;
            in_word = true;
        }
    }
    if (line_len > count->max_line) count->max_line = line_len;
    return ferror(file) ? -1 : 0;
}

static void print_counts(const counts_t *count, const wc_options_t *o, const char *name) {
    if (o->lines) printf("%7llu", count->lines);
    if (o->words) printf("%7llu", count->words);
    if (o->chars) printf("%7llu", count->chars);
    if (o->bytes) printf("%7llu", count->bytes);
    if (o->max_line) printf("%7llu", count->max_line);
    if (name) printf(" %s", name);
    putchar('\n');
}

int main(int argc, char **argv) {
    kx_prog = "wc";
    wc_options_t o = {0};
    bool selected = false;
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: wc [-clmwL] [FILE...]");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            selected = true;
            switch (*p) {
            case 'c': o.bytes = true; break;
            case 'l': o.lines = true; break;
            case 'm': o.chars = true; break;
            case 'w': o.words = true; break;
            case 'L': o.max_line = true; break;
            default: kx_die("invalid option");
            }
        }
    }
    if (!selected) o.lines = o.words = o.bytes = true;

    counts_t total = {0};
    int files = argc - first;
    int streams = files ? files : 1;
    int rc = 0;
    for (int i = 0; i < streams; i++) {
        const char *name = files ? argv[first + i] : NULL;
        FILE *file = !name || strcmp(name, "-") == 0 ? stdin : fopen(name, "rb");
        if (!file) {
            kx_warn(name);
            rc = 1;
            continue;
        }
        counts_t count = {0};
        if (count_stream(file, &count) < 0) {
            kx_warn(name ? name : "stdin");
            rc = 1;
        } else {
            print_counts(&count, &o, name);
            total.lines += count.lines;
            total.words += count.words;
            total.bytes += count.bytes;
            total.chars += count.chars;
            if (count.max_line > total.max_line) total.max_line = count.max_line;
        }
        if (file != stdin) fclose(file);
    }
    if (files > 1) print_counts(&total, &o, "total");
    return rc;
}
