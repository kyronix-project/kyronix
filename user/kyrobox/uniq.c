#include "common.h"

typedef struct {
    bool count;
    bool repeated;
    bool unique;
    bool ignore_case;
    bool zero;
    unsigned long skip_fields;
    unsigned long skip_chars;
    unsigned long width;
    bool have_width;
} uniq_options_t;

static const char *comparison_start(const char *line, const uniq_options_t *o) {
    const char *p = line;
    for (unsigned long field = 0; field < o->skip_fields; field++) {
        while (*p && isspace((unsigned char) *p)) p++;
        while (*p && !isspace((unsigned char) *p)) p++;
    }
    for (unsigned long i = 0; i < o->skip_chars && *p; i++) p++;
    return p;
}

static bool same_line(const char *left, const char *right, const uniq_options_t *o) {
    left = comparison_start(left, o);
    right = comparison_start(right, o);
    size_t width = o->have_width ? o->width : SIZE_MAX;
    if (o->ignore_case) return strncasecmp(left, right, width) == 0;
    return strncmp(left, right, width) == 0;
}

static void write_group(FILE *output, const char *line, unsigned long long count,
                        const uniq_options_t *o) {
    if (o->repeated && count < 2) return;
    if (o->unique && count != 1) return;
    if (o->count) fprintf(output, "%7llu ", count);
    if (o->zero) {
        size_t len = strlen(line);
        if (len && line[len - 1] == '\n') len--;
        fwrite(line, 1, len, output);
        putc('\0', output);
    } else
        fputs(line, output);
}

static unsigned long option_number(const char *attached, int *index, int argc, char **argv) {
    const char *value = attached;
    if (!*value) {
        if (++*index >= argc) kx_die("option requires an argument");
        value = argv[*index];
    }
    char *end = NULL;
    unsigned long result = strtoul(value, &end, 10);
    if (!end || *end) kx_die("invalid number");
    return result;
}

int main(int argc, char **argv) {
    kx_prog = "uniq";
    uniq_options_t o = {0};
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: uniq [-cduiz] [-f N] [-s N] [-w N] [INPUT [OUTPUT]]");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'c': o.count = true; break;
            case 'd': o.repeated = true; o.unique = false; break;
            case 'u': o.unique = true; o.repeated = false; break;
            case 'i': o.ignore_case = true; break;
            case 'z': o.zero = true; break;
            case 'f':
                o.skip_fields = option_number(p + 1, &first, argc, argv);
                p += strlen(p + 1);
                break;
            case 's':
                o.skip_chars = option_number(p + 1, &first, argc, argv);
                p += strlen(p + 1);
                break;
            case 'w':
                o.width = option_number(p + 1, &first, argc, argv);
                o.have_width = true;
                p += strlen(p + 1);
                break;
            default: kx_die("invalid option");
            }
        }
    }
    if (argc - first > 2) kx_die("extra operand");

    FILE *input = first < argc && strcmp(argv[first], "-") != 0 ? fopen(argv[first], "r") : stdin;
    if (!input) {
        kx_warn(argv[first]);
        return 1;
    }
    FILE *output = first + 1 < argc ? fopen(argv[first + 1], "w") : stdout;
    if (!output) {
        kx_warn(argv[first + 1]);
        if (input != stdin) fclose(input);
        return 1;
    }

    char *line = NULL;
    char *previous = NULL;
    size_t capacity = 0;
    unsigned long long count = 0;
    while (getline(&line, &capacity, input) >= 0) {
        if (previous && same_line(previous, line, &o)) {
            count++;
            continue;
        }
        if (previous) write_group(output, previous, count, &o);
        free(previous);
        previous = strdup(line);
        if (!previous) kx_die("out of memory");
        count = 1;
    }
    if (previous) write_group(output, previous, count, &o);
    free(previous);
    free(line);
    int rc = ferror(input) || ferror(output);
    if (input != stdin) fclose(input);
    if (output != stdout) fclose(output);
    return rc ? 1 : 0;
}
