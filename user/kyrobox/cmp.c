#include "common.h"

static void usage(void) {
    fprintf(stderr, "usage: cmp [-bls] [-n LIMIT] [-i SKIP1[:SKIP2]] FILE1 FILE2\n");
}

static bool number(const char *s, unsigned long long *out) {
    char *end;
    errno = 0;
    unsigned long long value = strtoull(s, &end, 0);
    if (end == s || *end || errno == ERANGE) return false;
    *out = value;
    return true;
}

static int skip_bytes(FILE *f, unsigned long long count) {
    if (count <= (unsigned long long) LLONG_MAX &&
        fseeko(f, (off_t) count, SEEK_SET) == 0)
        return 0;
    clearerr(f);
    while (count--) {
        if (fgetc(f) == EOF) return ferror(f) ? -1 : 0;
    }
    return 0;
}

static void print_char(int c) {
    if (isprint((unsigned char) c))
        printf("%c", c);
    else
        printf("\\%03o", (unsigned char) c);
}

int main(int argc, char **argv) {
    kx_prog = "cmp";
    bool print_bytes = false, list = false, silent = false;
    unsigned long long limit = ULLONG_MAX, skip1 = 0, skip2 = 0;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--silent") == 0 || strcmp(a, "--quiet") == 0) {
            silent = true;
            continue;
        }
        if (strncmp(a, "--bytes=", 8) == 0) {
            if (!number(a + 8, &limit)) kx_die("invalid byte limit");
            continue;
        }
        if (strncmp(a, "--ignore-initial=", 17) == 0) {
            a += 17;
            char tmp[64];
            if (strlen(a) >= sizeof(tmp)) kx_die("invalid skip value");
            strcpy(tmp, a);
            char *colon = strchr(tmp, ':');
            if (colon) *colon++ = '\0';
            if (!number(tmp, &skip1) || (colon && !number(colon, &skip2)))
                kx_die("invalid skip value");
            if (!colon) skip2 = skip1;
            continue;
        }
        if (a[1] == 'n' || a[1] == 'i') {
            char opt = a[1];
            const char *value = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!value) {
                usage();
                return 2;
            }
            if (opt == 'n') {
                if (!number(value, &limit)) kx_die("invalid byte limit");
            } else {
                char tmp[64];
                if (strlen(value) >= sizeof(tmp)) kx_die("invalid skip value");
                strcpy(tmp, value);
                char *colon = strchr(tmp, ':');
                if (colon) *colon++ = '\0';
                if (!number(tmp, &skip1) || (colon && !number(colon, &skip2)))
                    kx_die("invalid skip value");
                if (!colon) skip2 = skip1;
            }
            continue;
        }
        for (const char *o = a + 1; *o; o++) {
            if (*o == 'b') print_bytes = true;
            else if (*o == 'l') list = true;
            else if (*o == 's') silent = true;
            else {
                usage();
                return 2;
            }
        }
    }
    if (argc - i < 2 || argc - i > 4) {
        usage();
        return 2;
    }
    const char *name1 = argv[i++], *name2 = argv[i++];
    if (i < argc && !number(argv[i++], &skip1)) kx_die("invalid skip value");
    if (i < argc && !number(argv[i++], &skip2)) kx_die("invalid skip value");
    if (strcmp(name1, "-") == 0 && strcmp(name2, "-") == 0) kx_die("both files cannot be stdin");

    FILE *a = strcmp(name1, "-") == 0 ? stdin : fopen(name1, "rb");
    if (!a) {
        if (!silent) kx_warn(name1);
        return 2;
    }
    FILE *b = strcmp(name2, "-") == 0 ? stdin : fopen(name2, "rb");
    if (!b) {
        if (!silent) kx_warn(name2);
        if (a != stdin) fclose(a);
        return 2;
    }
    if (skip_bytes(a, skip1) < 0 || skip_bytes(b, skip2) < 0) {
        if (!silent) kx_warn("seek");
        if (a != stdin) fclose(a);
        if (b != stdin) fclose(b);
        return 2;
    }

    unsigned long long byte = 1, line_no = 1, compared = 0;
    int different = 0;
    while (compared < limit) {
        int ca = fgetc(a), cb = fgetc(b);
        if (ca == EOF || cb == EOF) {
            if (ferror(a) || ferror(b)) {
                different = 2;
            } else if (ca != cb) {
                different = 1;
                if (!silent && !list)
                    fprintf(stderr, "cmp: EOF on %s after byte %llu\n",
                            ca == EOF ? name1 : name2, byte - 1);
            }
            break;
        }
        compared++;
        if (ca != cb) {
            different = 1;
            if (list && !silent) {
                printf("%llu %03o %03o\n", byte, (unsigned char) ca, (unsigned char) cb);
            } else if (!silent) {
                printf("%s %s differ: byte %llu, line %llu", name1, name2, byte, line_no);
                if (print_bytes) {
                    fputs(" is ", stdout);
                    print_char(ca);
                    fputs(" ", stdout);
                    print_char(cb);
                }
                putchar('\n');
                break;
            } else {
                break;
            }
        }
        if (ca == '\n') line_no++;
        byte++;
    }
    if (a != stdin) fclose(a);
    if (b != stdin) fclose(b);
    return different;
}
