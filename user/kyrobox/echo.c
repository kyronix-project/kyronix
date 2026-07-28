#include "common.h"

static bool print_escaped(const char *s) {
    for (const char *p = s; *p; p++) {
        if (*p != '\\') {
            putchar((unsigned char) *p);
            continue;
        }
        p++;
        if (!*p) {
            putchar('\\');
            break;
        }
        int c;
        switch (*p) {
            case 'a': c = '\a'; break;
            case 'b': c = '\b'; break;
            case 'c': return false;
            case 'e':
            case 'E': c = 27; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'v': c = '\v'; break;
            case '\\': c = '\\'; break;
            case '0': {
                unsigned value = 0;
                int digits = 0;
                while (digits < 3 && p[1] >= '0' && p[1] <= '7') {
                    value = value * 8 + (unsigned) (*++p - '0');
                    digits++;
                }
                c = (unsigned char) value;
                break;
            }
            case 'x': {
                unsigned value = 0;
                int digits = 0;
                while (digits < 2 && isxdigit((unsigned char) p[1])) {
                    char h = *++p;
                    value = value * 16 +
                            (unsigned) (isdigit((unsigned char) h) ? h - '0'
                                                                 : tolower((unsigned char) h) - 'a' + 10);
                    digits++;
                }
                if (!digits) {
                    putchar('\\');
                    c = 'x';
                } else {
                    c = (unsigned char) value;
                }
                break;
            }
            default:
                putchar('\\');
                c = (unsigned char) *p;
                break;
        }
        putchar(c);
    }
    return true;
}

int main(int argc, char **argv) {
    bool newline = true, escapes = false;
    int i = 1;
    for (; i < argc; i++) {
        if (argv[i][0] != '-' || argv[i][1] == '\0') break;
        bool valid = true;
        for (const char *p = argv[i] + 1; *p; p++)
            if (*p != 'n' && *p != 'e' && *p != 'E') valid = false;
        if (!valid) break;
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'n') newline = false;
            else if (*p == 'e') escapes = true;
            else if (*p == 'E') escapes = false;
        }
    }
    bool first_operand = true;
    for (; i < argc; i++) {
        if (!first_operand) putchar(' ');
        first_operand = false;
        if (escapes) {
            if (!print_escaped(argv[i])) {
                newline = false;
                break;
            }
        } else {
            fputs(argv[i], stdout);
        }
    }
    if (newline) putchar('\n');
    return ferror(stdout) ? 1 : 0;
}
