#include "common.h"

typedef struct {
    bool number;
    bool number_nonblank;
    bool squeeze;
    bool show_ends;
    bool show_tabs;
    bool show_nonprinting;
    unsigned long line;
    bool at_line_start;
    bool previous_blank;
} cat_options_t;

static void show_byte(unsigned char c, const cat_options_t *o) {
    if (c == '\t' && o->show_tabs) {
        fputs("^I", stdout);
    } else if (o->show_nonprinting && c < 32 && c != '\n' && c != '\t') {
        putchar('^');
        putchar((char) (c + 64));
    } else if (o->show_nonprinting && c == 127) {
        fputs("^?", stdout);
    } else if (o->show_nonprinting && c >= 128) {
        fputs("M-", stdout);
        c &= 0x7f;
        if (c < 32) {
            putchar('^');
            putchar((char) (c + 64));
        } else if (c == 127) {
            fputs("^?", stdout);
        } else {
            putchar(c);
        }
    } else {
        putchar(c);
    }
}

static int copy_stream(FILE *input, cat_options_t *o) {
    int c;
    while ((c = fgetc(input)) != EOF) {
        bool blank = o->at_line_start && c == '\n';
        if (blank && o->squeeze && o->previous_blank) continue;

        if (o->at_line_start && (o->number || (o->number_nonblank && !blank)))
            printf("%6lu\t", o->line++);

        if (c == '\n') {
            if (o->show_ends) putchar('$');
            putchar('\n');
            o->at_line_start = true;
            o->previous_blank = blank;
        } else {
            show_byte((unsigned char) c, o);
            o->at_line_start = false;
            o->previous_blank = false;
        }
    }
    return ferror(input) ? -1 : 0;
}

int main(int argc, char **argv) {
    kx_prog = "cat";
    cat_options_t o = { .line = 1, .at_line_start = true };
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: cat [-AbEnstTuv] [FILE...]");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'A': o.show_nonprinting = o.show_ends = o.show_tabs = true; break;
            case 'b': o.number_nonblank = true; o.number = false; break;
            case 'E': o.show_ends = true; break;
            case 'n': if (!o.number_nonblank) o.number = true; break;
            case 's': o.squeeze = true; break;
            case 'T': o.show_tabs = true; break;
            case 'v': o.show_nonprinting = true; break;
            case 'e': o.show_nonprinting = o.show_ends = true; break;
            case 't': o.show_nonprinting = o.show_tabs = true; break;
            case 'u': break;
            default: kx_die("invalid option");
            }
        }
    }

    int rc = 0;
    if (first == argc) return copy_stream(stdin, &o) < 0;
    for (int i = first; i < argc; i++) {
        FILE *input = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "rb");
        if (!input) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        if (copy_stream(input, &o) < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
        if (input != stdin) fclose(input);
    }
    return rc;
}
