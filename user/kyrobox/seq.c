#include "common.h"
#include <math.h>

static void usage(void) {
    fprintf(stderr, "usage: seq [-w] [-f FORMAT] [-s STRING] [FIRST [INCREMENT]] LAST\n");
}

static int decimals(const char *s) {
    const char *dot = strchr(s, '.');
    if (!dot) return 0;
    const char *end = strpbrk(dot, "eE");
    if (!end) end = s + strlen(s);
    return (int) (end - dot - 1);
}

static bool valid_format(const char *fmt) {
    int conversions = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (strchr("-+ #0", *p)) p++;
        while (isdigit((unsigned char) *p)) p++;
        if (*p == '.') {
            p++;
            while (isdigit((unsigned char) *p)) p++;
        }
        if (!strchr("aAeEfFgG", *p)) return false;
        conversions++;
    }
    return conversions == 1;
}

static bool parse_double(const char *s, double *out) {
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end || errno == ERANGE || !isfinite(v)) return false;
    *out = v;
    return true;
}

int main(int argc, char **argv) {
    kx_prog = "seq";
    const char *format = NULL, *separator = "\n";
    bool equal_width = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strcmp(a, "-w") == 0) {
            equal_width = true;
            continue;
        }
        if (strncmp(a, "-f", 2) == 0 || strncmp(a, "-s", 2) == 0) {
            char opt = a[1];
            const char *value = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!value) {
                usage();
                return 1;
            }
            if (opt == 'f') format = value;
            else separator = value;
            continue;
        }
        if (a[0] == '-' && a[1] && !isdigit((unsigned char) a[1]) && a[1] != '.') {
            usage();
            return 1;
        }
        break;
    }

    int operands = argc - i;
    if (operands < 1 || operands > 3 || (format && equal_width) ||
        (format && !valid_format(format))) {
        usage();
        return 1;
    }
    double first = 1.0, step = 1.0, last;
    const char *first_s = "1", *step_s = "1", *last_s;
    if (operands == 1) {
        last_s = argv[i];
        if (!parse_double(last_s, &last)) kx_die("invalid number");
    } else if (operands == 2) {
        first_s = argv[i];
        last_s = argv[i + 1];
        if (!parse_double(first_s, &first) || !parse_double(last_s, &last))
            kx_die("invalid number");
    } else {
        first_s = argv[i];
        step_s = argv[i + 1];
        last_s = argv[i + 2];
        if (!parse_double(first_s, &first) || !parse_double(step_s, &step) ||
            !parse_double(last_s, &last))
            kx_die("invalid number");
    }
    if (step == 0.0) kx_die("zero increment");

    int precision = decimals(first_s);
    if (decimals(step_s) > precision) precision = decimals(step_s);
    if (decimals(last_s) > precision) precision = decimals(last_s);
    char first_buf[128], last_buf[128];
    snprintf(first_buf, sizeof(first_buf), "%.*f", precision, first);
    snprintf(last_buf, sizeof(last_buf), "%.*f", precision, last);
    int width = (int) strlen(first_buf);
    if ((int) strlen(last_buf) > width) width = (int) strlen(last_buf);

    bool printed = false;
    for (unsigned long long n = 0; n < 1000000000ULL; n++) {
        double value = first + (double) n * step;
        double tolerance = fabs(step) * 1e-12 + 1e-12;
        if ((step > 0.0 && value > last + tolerance) ||
            (step < 0.0 && value < last - tolerance))
            break;
        if (printed && fputs(separator, stdout) == EOF) return 1;
        if (format)
            printf(format, value);
        else if (equal_width)
            printf("%0*.*f", width, precision, value);
        else if (precision > 0)
            printf("%.*f", precision, value);
        else
            printf("%.15g", value);
        printed = true;
    }
    if (printed) putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
