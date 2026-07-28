#include "common.h"

typedef struct {
    unsigned char v[256];
    size_t n;
} byte_set_t;

static void set_add(byte_set_t *set, unsigned char c) {
    if (set->n < sizeof(set->v)) set->v[set->n++] = c;
}

static int class_match(const char *name, unsigned char c) {
    if (strcmp(name, "alnum") == 0) return isalnum(c);
    if (strcmp(name, "alpha") == 0) return isalpha(c);
    if (strcmp(name, "blank") == 0) return c == ' ' || c == '\t';
    if (strcmp(name, "cntrl") == 0) return iscntrl(c);
    if (strcmp(name, "digit") == 0) return isdigit(c);
    if (strcmp(name, "graph") == 0) return isgraph(c);
    if (strcmp(name, "lower") == 0) return islower(c);
    if (strcmp(name, "print") == 0) return isprint(c);
    if (strcmp(name, "punct") == 0) return ispunct(c);
    if (strcmp(name, "space") == 0) return isspace(c);
    if (strcmp(name, "upper") == 0) return isupper(c);
    if (strcmp(name, "xdigit") == 0) return isxdigit(c);
    return 0;
}

static unsigned char escaped_char(const char **sp) {
    const char *s = *sp;
    if (*s != '\\') {
        unsigned char c = (unsigned char) *s++;
        *sp = s;
        return c;
    }
    s++;
    if (*s >= '0' && *s <= '7') {
        unsigned value = 0;
        int digits = 0;
        while (digits < 3 && *s >= '0' && *s <= '7') {
            value = value * 8 + (unsigned) (*s++ - '0');
            digits++;
        }
        *sp = s;
        return (unsigned char) value;
    }
    unsigned char c;
    switch (*s) {
        case 'a': c = '\a'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'n': c = '\n'; break;
        case 'r': c = '\r'; break;
        case 't': c = '\t'; break;
        case 'v': c = '\v'; break;
        case '\\': c = '\\'; break;
        case '\0': c = '\\'; s--; break;
        default: c = (unsigned char) *s; break;
    }
    if (*s) s++;
    *sp = s;
    return c;
}

static void expand_set(const char *spec, byte_set_t *out) {
    const char *p = spec;
    while (*p) {
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            if (end) {
                char name[16];
                size_t n = (size_t) (end - (p + 2));
                if (n < sizeof(name)) {
                    memcpy(name, p + 2, n);
                    name[n] = '\0';
                    bool known = strcmp(name, "alnum") == 0 || strcmp(name, "alpha") == 0 ||
                                 strcmp(name, "blank") == 0 || strcmp(name, "cntrl") == 0 ||
                                 strcmp(name, "digit") == 0 || strcmp(name, "graph") == 0 ||
                                 strcmp(name, "lower") == 0 || strcmp(name, "print") == 0 ||
                                 strcmp(name, "punct") == 0 || strcmp(name, "space") == 0 ||
                                 strcmp(name, "upper") == 0 || strcmp(name, "xdigit") == 0;
                    if (known) {
                        for (unsigned c = 0; c < 256; c++)
                            if (class_match(name, (unsigned char) c))
                                set_add(out, (unsigned char) c);
                        p = end + 2;
                        continue;
                    }
                }
            }
        }

        const char *next = p;
        unsigned char first = escaped_char(&next);
        if (*next == '-' && next[1]) {
            const char *after = next + 1;
            unsigned char last = escaped_char(&after);
            if (first <= last)
                for (unsigned c = first; c <= last; c++) set_add(out, (unsigned char) c);
            else
                for (int c = first; c >= last; c--) set_add(out, (unsigned char) c);
            p = after;
        } else {
            set_add(out, first);
            p = next;
        }
    }
}

static void complement_set(byte_set_t *set) {
    bool used[256] = { false };
    for (size_t i = 0; i < set->n; i++) used[set->v[i]] = true;
    byte_set_t result = { .n = 0 };
    for (unsigned c = 0; c < 256; c++)
        if (!used[c]) set_add(&result, (unsigned char) c);
    *set = result;
}

static void usage(void) {
    fprintf(stderr, "usage: tr [-cdst] SET1 [SET2]\n");
}

int main(int argc, char **argv) {
    kx_prog = "tr";
    bool complement = false, delete = false, squeeze = false, truncate = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (a[0] != '-' || a[1] == '\0') break;
        for (const char *o = a + 1; *o; o++) {
            if (*o == 'c' || *o == 'C') complement = true;
            else if (*o == 'd') delete = true;
            else if (*o == 's') squeeze = true;
            else if (*o == 't') truncate = true;
            else {
                usage();
                return 1;
            }
        }
    }

    int operands = argc - i;
    if (operands < 1 || (!delete && !squeeze && operands < 2) || operands > 2) {
        usage();
        return 1;
    }

    byte_set_t set1 = { .n = 0 }, set2 = { .n = 0 };
    expand_set(argv[i], &set1);
    if (operands == 2) expand_set(argv[i + 1], &set2);
    if (complement) complement_set(&set1);
    if (!delete && operands == 2 && set2.n == 0) kx_die("empty SET2");

    unsigned char map[256];
    bool remove[256] = { false }, squash[256] = { false };
    for (unsigned c = 0; c < 256; c++) map[c] = (unsigned char) c;
    if (delete) {
        for (size_t n = 0; n < set1.n; n++) remove[set1.v[n]] = true;
    } else if (operands == 2) {
        size_t nmap = truncate && set1.n > set2.n ? set2.n : set1.n;
        for (size_t n = 0; n < nmap; n++)
            map[set1.v[n]] = set2.v[n < set2.n ? n : set2.n - 1];
    }
    if (squeeze) {
        byte_set_t *sq = (!delete && operands == 2) ? &set2 : &set1;
        for (size_t n = 0; n < sq->n; n++) squash[sq->v[n]] = true;
    }

    int c, previous = -1;
    while ((c = getchar()) != EOF) {
        unsigned char in = (unsigned char) c;
        if (remove[in]) continue;
        unsigned char out = map[in];
        if (squeeze && previous == out && squash[out]) continue;
        if (putchar(out) == EOF) return 1;
        previous = out;
    }
    return ferror(stdin) || ferror(stdout) ? 1 : 0;
}
