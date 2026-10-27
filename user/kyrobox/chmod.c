#include "common.h"

static int parse_symbolic(const char *mode_str, mode_t current_mode) {
    const char *p = mode_str;
    mode_t new_mode = current_mode;

    while (*p) {
        mode_t who = 0;
        bool got_who = false;
        while (*p == 'u' || *p == 'g' || *p == 'o' || *p == 'a') {
            got_who = true;
            switch (*p) {
            case 'u':
                who |= 0700;
                break;
            case 'g':
                who |= 0070;
                break;
            case 'o':
                who |= 0007;
                break;
            case 'a':
                who |= 0777;
                break;
            }
            p++;
        }
        if (!got_who) who = 0777;

        if (*p != '+' && *p != '-' && *p != '=') return -1;
        char op = *p++;

        mode_t add = 0, remove = 0;

        if (!*p || *p == ',' || *p == '+' || *p == '-' || *p == '=') {
            if (op == '+')
                new_mode |= who;
            else if (op == '-')
                new_mode &= ~who;
            else if (op == '=')
                new_mode = (new_mode & ~07777U) | (who & 0777U);
            if (*p == ',') p++;
            continue;
        }

        while (*p && *p != ',' && *p != '+' && *p != '-' && *p != '=') {
            mode_t bit = 0;
            switch (*p) {
            case 'r':
                bit = 0444;
                break;
            case 'w':
                bit = 0222;
                break;
            case 'x':
                bit = 0111;
                break;
            case 's':
                if (who & 0700) bit |= 04000;
                if (who & 0070) bit |= 02000;
                break;
            case 't':
                bit = 01000;
                break;
            default:
                return -1;
            }
            if (op == '+')
                add |= (bit & (who ? who : 07777U));
            else if (op == '-')
                remove |= (bit & (who ? who : 07777U));
            else if (op == '=')
                add |= (bit & (who ? who : 07777U));
            p++;
        }

        new_mode |= add;
        new_mode &= ~remove;
        if (op == '=') {
            new_mode = (current_mode & ~0777U) | (add & 0777U);
            if (who & 04000) new_mode |= 04000;
            if (who & 02000) new_mode |= 02000;
            if (who & 01000) new_mode |= 01000;
        }
        if (*p == ',') p++;
    }
    return (int) (new_mode & 07777U);
}

int main(int argc, char **argv) {
    kx_prog = "chmod";
    if (argc < 3) kx_die("usage: chmod MODE FILE...");

    char *end = NULL;
    long mode = strtol(argv[1], &end, 8);
    bool is_octal = (end && *end == '\0');

    int rc = 0;
    for (int i = 2; i < argc; i++) {
        struct stat st;
        if (stat(argv[i], &st) < 0) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        mode_t final_mode;
        if (is_octal) {
            final_mode = (mode_t) mode;
        } else {
            int sym = parse_symbolic(argv[1], st.st_mode);
            if (sym < 0) kx_die("bad mode");
            final_mode = (mode_t) sym;
        }
        if (chmod(argv[i], final_mode) < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
