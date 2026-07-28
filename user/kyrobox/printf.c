#include "common.h"

static bool stop_output;

static int hex_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *print_escape(const char *p, bool percent_b) {
    unsigned char c;
    if (*p != '\\') {
        putchar((unsigned char) *p);
        return p + (*p != '\0');
    }
    p++;
    switch (*p) {
        case 'a': c = '\a'; p++; break;
        case 'b': c = '\b'; p++; break;
        case 'c': stop_output = true; return p + 1;
        case 'e': c = 27; p++; break;
        case 'f': c = '\f'; p++; break;
        case 'n': c = '\n'; p++; break;
        case 'r': c = '\r'; p++; break;
        case 't': c = '\t'; p++; break;
        case 'v': c = '\v'; p++; break;
        case '\\': c = '\\'; p++; break;
        case '"': c = '"'; p++; break;
        case 'x': {
            p++;
            int value = 0, digits = 0, h;
            while (digits < 2 && (h = hex_value((unsigned char) *p)) >= 0) {
                value = value * 16 + h;
                p++;
                digits++;
            }
            c = digits ? (unsigned char) value : 'x';
            break;
        }
        case '0': {
            p++;
            int value = 0, digits = 0;
            while (digits < 3 && *p >= '0' && *p <= '7') {
                value = value * 8 + (*p++ - '0');
                digits++;
            }
            c = (unsigned char) value;
            break;
        }
        default:
            if (percent_b && *p >= '0' && *p <= '7') {
                int value = 0, digits = 0;
                while (digits < 3 && *p >= '0' && *p <= '7') {
                    value = value * 8 + (*p++ - '0');
                    digits++;
                }
                c = (unsigned char) value;
            } else {
                putchar('\\');
                if (!*p) return p;
                c = (unsigned char) *p++;
            }
            break;
    }
    putchar(c);
    return p;
}

static void print_b(const char *s) {
    for (const char *p = s; *p && !stop_output;) p = print_escape(p, true);
}

static long long signed_arg(const char *s) {
    if ((s[0] == '\'' || s[0] == '"') && s[1]) return (unsigned char) s[1];
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 0);
    if (end == s || *end || errno == ERANGE) {
        fprintf(stderr, "printf: %s: expected numeric value\n", s);
        return 0;
    }
    return v;
}

static unsigned long long unsigned_arg(const char *s) {
    return (unsigned long long) signed_arg(s);
}

static double float_arg(const char *s) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno == ERANGE) {
        fprintf(stderr, "printf: %s: expected numeric value\n", s);
        return 0.0;
    }
    return v;
}

static int emit_format(const char *fmt, int argc, char **argv, int *argp) {
    int start_arg = *argp;
    for (const char *p = fmt; *p && !stop_output;) {
        if (*p == '\\') {
            p = print_escape(p, false);
            continue;
        }
        if (*p != '%') {
            putchar((unsigned char) *p++);
            continue;
        }
        const char *begin = p++;
        if (*p == '%') {
            putchar('%');
            p++;
            continue;
        }

        while (strchr("-+ #0", *p)) p++;
        if (*p == '*') kx_die("'*' width is not supported");
        while (isdigit((unsigned char) *p)) p++;
        if (*p == '.') {
            p++;
            if (*p == '*') kx_die("'*' precision is not supported");
            while (isdigit((unsigned char) *p)) p++;
        }
        char conversion = *p;
        if (!conversion) {
            fputs(begin, stdout);
            break;
        }
        p++;
        char spec[64];
        size_t prefix = (size_t) (p - begin - 1);
        if (prefix + 4 >= sizeof(spec)) kx_die("format conversion too long");
        memcpy(spec, begin, prefix);

        const char *arg = *argp < argc ? argv[(*argp)++] : "";
        if (conversion == 's' || conversion == 'c' || conversion == 'b') {
            spec[prefix] = conversion;
            spec[prefix + 1] = '\0';
            if (conversion == 's') printf(spec, arg);
            else if (conversion == 'c') {
                spec[prefix] = 'c';
                printf(spec, *arg ? (unsigned char) *arg : 0);
            } else
                print_b(arg);
        } else if (strchr("di", conversion)) {
            spec[prefix++] = 'l';
            spec[prefix++] = 'l';
            spec[prefix++] = conversion;
            spec[prefix] = '\0';
            printf(spec, signed_arg(arg));
        } else if (strchr("ouxX", conversion)) {
            spec[prefix++] = 'l';
            spec[prefix++] = 'l';
            spec[prefix++] = conversion;
            spec[prefix] = '\0';
            printf(spec, unsigned_arg(arg));
        } else if (strchr("aAeEfFgG", conversion)) {
            spec[prefix++] = conversion;
            spec[prefix] = '\0';
            printf(spec, float_arg(arg));
        } else {
            fprintf(stderr, "printf: invalid conversion %%%c\n", conversion);
            return -1;
        }
    }
    return *argp - start_arg;
}

int main(int argc, char **argv) {
    kx_prog = "printf";
    int first = 1;
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first == argc) return 0;
    const char *format = argv[first++];
    int arg = first;
    do {
        int consumed = emit_format(format, argc, argv, &arg);
        if (consumed < 0) return 1;
        if (consumed == 0) break;
    } while (arg < argc && !stop_output);
    return ferror(stdout) ? 1 : 0;
}
