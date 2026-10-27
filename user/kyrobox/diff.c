#include "common.h"
#include <sys/stat.h>

#define FMT_NORMAL 0
#define FMT_UNIFIED 1
#define FMT_CONTEXT 2
#define FMT_ED 3
#define FMT_SBS 4

typedef struct {
    int ignore_case;
    int ignore_space_change;
    int ignore_all_space;
    int ignore_blank_lines;
} cmpflags_t;

typedef struct {
    int format;
    int context;
    int recursive;
    int new_file;
    int brief;
    int report_identical;
    cmpflags_t cf;
    const char *label[2];
    int nlabels;
    int sbs_width;
    int had_trouble;
    int had_diff;
} opts_t;

typedef struct {
    char **lines;
    int n;
} filelines_t;

static int is_blank_line(const char *s) {
    while (*s) {
        if (!isspace((unsigned char) *s)) return 0;
        s++;
    }
    return 1;
}

static int line_eq(const char *a, const char *b, const cmpflags_t *f) {
    if (f->ignore_blank_lines && is_blank_line(a) && is_blank_line(b)) return 1;
    if (!f->ignore_case && !f->ignore_space_change && !f->ignore_all_space)
        return strcmp(a, b) == 0;

    const unsigned char *pa = (const unsigned char *) a;
    const unsigned char *pb = (const unsigned char *) b;
    for (;;) {
        if (f->ignore_all_space) {
            while (*pa && isspace(*pa)) pa++;
            while (*pb && isspace(*pb)) pb++;
        } else if (f->ignore_space_change && isspace(*pa) && isspace(*pb)) {
            while (isspace(*pa)) pa++;
            while (isspace(*pb)) pb++;
        }
        if (*pa == 0 || *pb == 0) return *pa == *pb;
        unsigned char ca = *pa, cb = *pb;
        if (f->ignore_case) {
            ca = (unsigned char) tolower(ca);
            cb = (unsigned char) tolower(cb);
        }
        if (ca != cb) return 0;
        pa++;
        pb++;
    }
}

static int read_lines(const char *path, filelines_t *fl, int allow_missing) {
    fl->lines = NULL;
    fl->n = 0;
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "r");
    if (!f) {
        if (allow_missing) return 0;
        kx_warn(path);
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int alloc = 0;
    while ((len = getline(&line, &cap, f)) >= 0) {
        if (len > 0 && line[(size_t) len - 1] == '\n') line[--len] = 0;
        if (fl->n >= alloc) {
            alloc = alloc ? alloc * 2 : 64;
            fl->lines = realloc(fl->lines, (size_t) alloc * sizeof(char *));
        }
        fl->lines[fl->n++] = strdup(line);
    }
    free(line);
    if (f != stdin) fclose(f);
    return 0;
}

static void free_lines(filelines_t *fl) {
    for (int i = 0; i < fl->n; i++) free(fl->lines[i]);
    free(fl->lines);
    fl->lines = NULL;
    fl->n = 0;
}

typedef enum { OP_EQ, OP_DEL, OP_INS } optype_t;

typedef struct {
    optype_t type;
    int ai, bi;
} edit_t;

static edit_t *build_edits(filelines_t *A, filelines_t *B, const cmpflags_t *cf, int *out_ne) {
    int n = A->n, m = B->n;
    int *dp = malloc((size_t) (n + 1) * (size_t) (m + 1) * sizeof(int));
#define DP(i, j) dp[(size_t) (i) * (size_t) (m + 1) + (size_t) (j)]
    for (int i = n; i >= 0; i--) {
        for (int j = m; j >= 0; j--) {
            if (i == n || j == m) {
                DP(i, j) = 0;
            } else if (line_eq(A->lines[i], B->lines[j], cf)) {
                DP(i, j) = DP(i + 1, j + 1) + 1;
            } else {
                int d1 = DP(i + 1, j), d2 = DP(i, j + 1);
                DP(i, j) = d1 > d2 ? d1 : d2;
            }
        }
    }

    edit_t *edits = malloc((size_t) (n + m + 1) * sizeof(edit_t));
    int ne = 0, i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && line_eq(A->lines[i], B->lines[j], cf)) {
            edits[ne++] = (edit_t) { OP_EQ, i, j };
            i++;
            j++;
        } else if (j < m && (i == n || DP(i, j + 1) > DP(i + 1, j))) {
            edits[ne++] = (edit_t) { OP_INS, i, j };
            j++;
        } else {
            edits[ne++] = (edit_t) { OP_DEL, i, j };
            i++;
        }
    }
    free(dp);
    *out_ne = ne;
    return edits;
}

typedef struct {
    int hs, he;
} hunk_t;

static hunk_t *group_hunks(edit_t *edits, int ne, int ctx, int *out_n) {
    hunk_t *hunks = malloc((size_t) (ne + 1) * sizeof(hunk_t));
    int nh = 0, k = 0;
    while (k < ne) {
        if (edits[k].type == OP_EQ) {
            k++;
            continue;
        }
        int hs = k, back = 0;
        while (hs > 0 && back < ctx && edits[hs - 1].type == OP_EQ) {
            hs--;
            back++;
        }
        int he = k;
        for (;;) {
            while (he < ne && edits[he].type != OP_EQ) he++;
            int run = 0, p2 = he;
            while (p2 < ne && edits[p2].type == OP_EQ && run < 2 * ctx) {
                p2++;
                run++;
            }
            if (p2 < ne && edits[p2].type != OP_EQ) {
                he = p2;
                continue;
            }
            break;
        }
        int fwd = 0;
        while (he < ne && fwd < ctx && edits[he].type == OP_EQ) {
            he++;
            fwd++;
        }
        hunks[nh++] = (hunk_t) { hs, he };
        k = he;
    }
    *out_n = nh;
    return hunks;
}

static void hunk_counts(edit_t *edits, hunk_t h, int *a_count, int *b_count) {
    *a_count = 0;
    *b_count = 0;
    for (int q = h.hs; q < h.he; q++) {
        if (edits[q].type == OP_EQ) {
            (*a_count)++;
            (*b_count)++;
        } else if (edits[q].type == OP_DEL) {
            (*a_count)++;
        } else {
            (*b_count)++;
        }
    }
}

static void hunk_range(edit_t *edits, hunk_t h, int *a_start, int *a_count, int *b_start,
                       int *b_count) {
    hunk_counts(edits, h, a_count, b_count);
    *a_start = *a_count > 0 ? edits[h.hs].ai + 1 : edits[h.hs].ai;
    *b_start = *b_count > 0 ? edits[h.hs].bi + 1 : edits[h.hs].bi;
}

static void print_range(int start, int count) {
    if (count == 0)
        printf("%d,%d", start, start);
    else if (count == 1)
        printf("%d", start);
    else
        printf("%d,%d", start, start + count - 1);
}

static void print_normal(filelines_t *A, filelines_t *B, edit_t *edits, hunk_t *hunks, int nh) {
    for (int h = 0; h < nh; h++) {
        int as, ac, bs, bc;
        hunk_range(edits, hunks[h], &as, &ac, &bs, &bc);
        char cmd = (ac > 0 && bc > 0) ? 'c' : (ac > 0 ? 'd' : 'a');
        if (ac > 0)
            print_range(as, ac);
        else
            printf("%d", as);
        putchar(cmd);
        if (bc > 0)
            print_range(bs, bc);
        else
            printf("%d", bs);
        putchar('\n');
        if (ac > 0)
            for (int q = hunks[h].hs; q < hunks[h].he; q++)
                if (edits[q].type == OP_DEL) printf("< %s\n", A->lines[edits[q].ai]);
        if (ac > 0 && bc > 0) printf("---\n");
        if (bc > 0)
            for (int q = hunks[h].hs; q < hunks[h].he; q++)
                if (edits[q].type == OP_INS) printf("> %s\n", B->lines[edits[q].bi]);
    }
}

static void print_ed(filelines_t *B, edit_t *edits, hunk_t *hunks, int nh) {
    for (int h = nh - 1; h >= 0; h--) {
        int as, ac, bs, bc;
        hunk_range(edits, hunks[h], &as, &ac, &bs, &bc);
        if (ac > 0 && bc > 0) {
            print_range(as, ac);
            printf("c\n");
        } else if (ac > 0) {
            print_range(as, ac);
            printf("d\n");
            continue;
        } else {
            printf("%da\n", as);
        }
        for (int q = hunks[h].hs; q < hunks[h].he; q++)
            if (edits[q].type == OP_INS) printf("%s\n", B->lines[edits[q].bi]);
        printf(".\n");
    }
}

static void print_unified(filelines_t *A, filelines_t *B, edit_t *edits, hunk_t *hunks, int nh,
                          const char *patha, const char *pathb, opts_t *o) {
    printf("--- %s\n", o->nlabels > 0 ? o->label[0] : patha);
    printf("+++ %s\n", o->nlabels > 1 ? o->label[1] : pathb);
    for (int h = 0; h < nh; h++) {
        int as, ac, bs, bc;
        hunk_range(edits, hunks[h], &as, &ac, &bs, &bc);
        printf("@@ -%d,%d +%d,%d @@\n", as, ac, bs, bc);
        for (int q = hunks[h].hs; q < hunks[h].he; q++) {
            if (edits[q].type == OP_EQ)
                printf(" %s\n", A->lines[edits[q].ai]);
            else if (edits[q].type == OP_DEL)
                printf("-%s\n", A->lines[edits[q].ai]);
            else
                printf("+%s\n", B->lines[edits[q].bi]);
        }
    }
}

static int hunk_has_del(edit_t *edits, int s, int e) {
    for (int q = s; q < e; q++)
        if (edits[q].type == OP_DEL) return 1;
    return 0;
}
static int hunk_has_ins(edit_t *edits, int s, int e) {
    for (int q = s; q < e; q++)
        if (edits[q].type == OP_INS) return 1;
    return 0;
}

static void print_context(filelines_t *A, filelines_t *B, edit_t *edits, hunk_t *hunks, int nh,
                          const char *patha, const char *pathb, opts_t *o) {
    printf("*** %s\n", o->nlabels > 0 ? o->label[0] : patha);
    printf("--- %s\n", o->nlabels > 1 ? o->label[1] : pathb);
    for (int h = 0; h < nh; h++) {
        int as, ac, bs, bc;
        hunk_range(edits, hunks[h], &as, &ac, &bs, &bc);
        printf("***************\n");
        printf("*** ");
        print_range(as, ac);
        printf(" ****\n");

        if (ac > 0) {
            for (int p = hunks[h].hs; p < hunks[h].he;) {
                if (edits[p].type == OP_EQ) {
                    printf("  %s\n", A->lines[edits[p].ai]);
                    p++;
                    continue;
                }
                if (edits[p].type == OP_INS) {
                    p++;
                    continue;
                }
                int rs = p;
                while (rs < hunks[h].he && edits[rs].type != OP_EQ) rs++;
                int mixed = hunk_has_del(edits, p, rs) && hunk_has_ins(edits, p, rs);
                for (int t = p; t < rs; t++)
                    if (edits[t].type == OP_DEL)
                        printf("%s %s\n", mixed ? "!" : "-", A->lines[edits[t].ai]);
                p = rs;
            }
        }

        printf("--- ");
        print_range(bs, bc);
        printf(" ----\n");
        if (bc > 0) {
            for (int p = hunks[h].hs; p < hunks[h].he;) {
                if (edits[p].type == OP_EQ) {
                    printf("  %s\n", B->lines[edits[p].bi]);
                    p++;
                    continue;
                }
                int rs = p;
                while (rs < hunks[h].he && edits[rs].type != OP_EQ) rs++;
                int mixed = hunk_has_del(edits, p, rs) && hunk_has_ins(edits, p, rs);
                for (int t = p; t < rs; t++)
                    if (edits[t].type == OP_INS)
                        printf("%s %s\n", mixed ? "!" : "+", B->lines[edits[t].bi]);
                p = rs;
            }
        }
    }
}

static void sbs_line(const char *l, const char *r, char mark, int width) {
    int half = (width - 3) / 2;
    if (half < 1) half = 1;
    int llen = l ? (int) strlen(l) : 0;
    printf("%.*s%*s %c %s\n", llen < half ? llen : half, l ? l : "", llen < half ? half - llen : 0,
           "", mark, r ? r : "");
}

static void print_sbs(filelines_t *A, filelines_t *B, edit_t *edits, int ne, opts_t *o) {
    int k = 0;
    while (k < ne) {
        if (edits[k].type == OP_EQ) {
            sbs_line(A->lines[edits[k].ai], B->lines[edits[k].bi], ' ', o->sbs_width);
            k++;
            continue;
        }
        int rs = k;
        while (k < ne && edits[k].type != OP_EQ) k++;
        int mixed = hunk_has_del(edits, rs, k) && hunk_has_ins(edits, rs, k);
        int di = rs, ii = rs;
        while (di < k && edits[di].type != OP_DEL) di++;
        while (ii < k && edits[ii].type != OP_INS) ii++;
        for (;;) {
            while (di < k && edits[di].type != OP_DEL) di++;
            while (ii < k && edits[ii].type != OP_INS) ii++;
            const char *l = di < k ? A->lines[edits[di].ai] : NULL;
            const char *r = ii < k ? B->lines[edits[ii].bi] : NULL;
            if (!l && !r) break;
            char mark = mixed && l && r ? '|' : (l ? '<' : '>');
            sbs_line(l, r, mark, o->sbs_width);
            if (di < k) di++;
            if (ii < k) ii++;
        }
    }
}

static int diff_one(const char *patha, const char *pathb, opts_t *o) {
    filelines_t A = { 0 }, B = { 0 };
    struct stat sa, sb;
    int have_a = stat(patha, &sa) == 0, have_b = stat(pathb, &sb) == 0;

    if ((!have_a || !have_b) && !o->new_file) {
        if (!have_a) fprintf(stderr, "%s: %s: No such file or directory\n", kx_prog, patha);
        if (!have_b) fprintf(stderr, "%s: %s: No such file or directory\n", kx_prog, pathb);
        o->had_trouble = 1;
        return 2;
    }

    if (read_lines(have_a || !o->new_file ? patha : "/dev/null", &A, !have_a) < 0 ||
        read_lines(have_b || !o->new_file ? pathb : "/dev/null", &B, !have_b) < 0) {
        o->had_trouble = 1;
        return 2;
    }

    int ne;
    edit_t *edits = build_edits(&A, &B, &o->cf, &ne);

    int changed = 0;
    for (int k = 0; k < ne; k++)
        if (edits[k].type != OP_EQ) changed = 1;

    if (!changed) {
        if (o->report_identical) printf("Files %s and %s are identical\n", patha, pathb);
        free_lines(&A);
        free_lines(&B);
        free(edits);
        return 0;
    }

    o->had_diff = 1;
    if (o->brief) {
        printf("Files %s and %s differ\n", patha, pathb);
        free_lines(&A);
        free_lines(&B);
        free(edits);
        return 1;
    }

    if (o->format == FMT_SBS) {
        print_sbs(&A, &B, edits, ne, o);
    } else {
        int ctx = (o->format == FMT_UNIFIED || o->format == FMT_CONTEXT) ? o->context : 0;
        int nh;
        hunk_t *hunks = group_hunks(edits, ne, ctx, &nh);
        switch (o->format) {
        case FMT_UNIFIED:
            print_unified(&A, &B, edits, hunks, nh, patha, pathb, o);
            break;
        case FMT_CONTEXT:
            print_context(&A, &B, edits, hunks, nh, patha, pathb, o);
            break;
        case FMT_ED:
            print_ed(&B, edits, hunks, nh);
            break;
        default:
            print_normal(&A, &B, edits, hunks, nh);
            break;
        }
        free(hunks);
    }

    free_lines(&A);
    free_lines(&B);
    free(edits);
    return 1;
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *) a, *(const char *const *) b);
}

static char **list_dir(const char *path, int *out_n) {
    DIR *d = opendir(path);
    if (!d) {
        *out_n = 0;
        return NULL;
    }
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (n >= cap) {
            cap = cap ? cap * 2 : 32;
            names = realloc(names, (size_t) cap * sizeof(char *));
        }
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t) n, sizeof(char *), cmp_str);
    *out_n = n;
    return names;
}

static int diff_tree(const char *da, const char *db, opts_t *o) {
    int na, nb;
    char **la = list_dir(da, &na);
    char **lb = list_dir(db, &nb);
    int i = 0, j = 0, rc = 0;

    while (i < na || j < nb) {
        int c = (i >= na) ? 1 : (j >= nb) ? -1 : strcmp(la[i], lb[j]);
        char pa[1024], pb[1024];
        if (c == 0) {
            snprintf(pa, sizeof pa, "%s/%s", da, la[i]);
            snprintf(pb, sizeof pb, "%s/%s", db, lb[j]);
            struct stat sa, sb;
            int isda = stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode);
            int isdb = stat(pb, &sb) == 0 && S_ISDIR(sb.st_mode);
            if (isda && isdb) {
                if (o->recursive) {
                    if (diff_tree(pa, pb, o)) rc = 1;
                } else {
                    printf("Common subdirectories: %s and %s\n", pa, pb);
                }
            } else if (isda != isdb) {
                fprintf(stderr, "%s: %s is a %s while %s is a %s\n", kx_prog, pa,
                        isda ? "directory" : "file", pb, isdb ? "directory" : "file");
                rc = 1;
            } else {
                int r = diff_one(pa, pb, o);
                if (r == 1) rc = 1;
                if (r == 2) rc = 1;
            }
            i++;
            j++;
        } else if (c < 0) {
            snprintf(pa, sizeof pa, "%s/%s", da, la[i]);
            struct stat sa;
            int isdir = stat(pa, &sa) == 0 && S_ISDIR(sa.st_mode);
            if (o->new_file) {
                snprintf(pb, sizeof pb, "%s/%s", db, la[i]);
                if (!isdir) diff_one(pa, pb, o);
                rc = 1;
            } else {
                printf("Only in %s: %s\n", da, la[i]);
                rc = 1;
            }
            i++;
        } else {
            snprintf(pb, sizeof pb, "%s/%s", db, lb[j]);
            struct stat sb;
            int isdir = stat(pb, &sb) == 0 && S_ISDIR(sb.st_mode);
            if (o->new_file) {
                snprintf(pa, sizeof pa, "%s/%s", da, lb[j]);
                if (!isdir) diff_one(pa, pb, o);
                rc = 1;
            } else {
                printf("Only in %s: %s\n", db, lb[j]);
                rc = 1;
            }
            j++;
        }
    }

    for (int k = 0; k < na; k++) free(la[k]);
    for (int k = 0; k < nb; k++) free(lb[k]);
    free(la);
    free(lb);
    return rc;
}

static void usage(void) {
    kx_die("usage: diff [-abBiNqrsuwy] [-c|-e|-n|-u] [-C N|-U N] [--label L]... FILE1 FILE2");
}

static int want_number(const char *s) {
    if (!*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isdigit((unsigned char) *p)) return 0;
    return 1;
}

int main(int argc, char **argv) {
    kx_prog = "diff";
    opts_t o = { 0 };
    o.format = FMT_NORMAL;
    o.context = 3;
    o.sbs_width = 130;

    char *pos[2];
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strcmp(a, "--") == 0) {
            while (++i < argc && npos < 2) pos[npos++] = argv[i];
            break;
        }
        if (strncmp(a, "--label=", 8) == 0) {
            if (o.nlabels < 2) o.label[o.nlabels++] = a + 8;
            continue;
        }
        if (strcmp(a, "--label") == 0 && i + 1 < argc) {
            if (o.nlabels < 2) o.label[o.nlabels++] = argv[++i];
            continue;
        }
        if (strncmp(a, "--context", 9) == 0) {
            o.format = FMT_CONTEXT;
            if (a[9] == '=') o.context = atoi(a + 10);
            continue;
        }
        if (strncmp(a, "--unified", 9) == 0) {
            o.format = FMT_UNIFIED;
            if (a[9] == '=') o.context = atoi(a + 10);
            continue;
        }
        if (strcmp(a, "--ed") == 0) {
            o.format = FMT_ED;
            continue;
        }
        if (strcmp(a, "--normal") == 0) {
            o.format = FMT_NORMAL;
            continue;
        }
        if (strcmp(a, "--side-by-side") == 0) {
            o.format = FMT_SBS;
            continue;
        }
        if (strcmp(a, "--brief") == 0) {
            o.brief = 1;
            continue;
        }
        if (strcmp(a, "--recursive") == 0) {
            o.recursive = 1;
            continue;
        }
        if (strcmp(a, "--new-file") == 0) {
            o.new_file = 1;
            continue;
        }
        if (strcmp(a, "--ignore-case") == 0) {
            o.cf.ignore_case = 1;
            continue;
        }
        if (strcmp(a, "--ignore-space-change") == 0) {
            o.cf.ignore_space_change = 1;
            continue;
        }
        if (strcmp(a, "--ignore-all-space") == 0) {
            o.cf.ignore_all_space = 1;
            continue;
        }
        if (strcmp(a, "--ignore-blank-lines") == 0) {
            o.cf.ignore_blank_lines = 1;
            continue;
        }
        if (strcmp(a, "--report-identical-files") == 0) {
            o.report_identical = 1;
            continue;
        }
        if (strcmp(a, "--text") == 0 || strcmp(a, "--expand-tabs") == 0 ||
            strcmp(a, "--initial-tab") == 0) {
            continue;
        }

        if (a[0] == '-' && a[1] != '-' && a[1] != '\0') {
            for (int ci = 1; a[ci]; ci++) {
                switch (a[ci]) {
                case 'a':
                case 't':
                case 'T':
                case 'p':
                    break;
                case 'b':
                    o.cf.ignore_space_change = 1;
                    break;
                case 'B':
                    o.cf.ignore_blank_lines = 1;
                    break;
                case 'i':
                    o.cf.ignore_case = 1;
                    break;
                case 'w':
                    o.cf.ignore_all_space = 1;
                    break;
                case 'N':
                    o.new_file = 1;
                    break;
                case 'q':
                    o.brief = 1;
                    break;
                case 'r':
                    o.recursive = 1;
                    break;
                case 's':
                    o.report_identical = 1;
                    break;
                case 'e':
                    o.format = FMT_ED;
                    break;
                case 'n':
                    o.format = FMT_NORMAL;
                    break;
                case 'y':
                    o.format = FMT_SBS;
                    break;
                case 'u':
                case 'c': {
                    o.format = a[ci] == 'u' ? FMT_UNIFIED : FMT_CONTEXT;
                    const char *rest = a + ci + 1;
                    if (want_number(rest) && *rest) {
                        o.context = atoi(rest);
                        ci = (int) strlen(a) - 1;
                    } else if (i + 1 < argc && want_number(argv[i + 1])) {
                        o.context = atoi(argv[++i]);
                    }
                    break;
                }
                case 'U':
                case 'C': {
                    o.format = a[ci] == 'U' ? FMT_UNIFIED : FMT_CONTEXT;
                    const char *rest = a + ci + 1;
                    if (want_number(rest) && *rest) {
                        o.context = atoi(rest);
                        ci = (int) strlen(a) - 1;
                    } else if (i + 1 < argc && want_number(argv[i + 1])) {
                        o.context = atoi(argv[++i]);
                    }
                    break;
                }
                default:
                    usage();
                }
            }
            continue;
        }

        if (npos < 2)
            pos[npos++] = a;
        else
            usage();
    }

    if (npos != 2) usage();

    struct stat sa, sb;
    int isda = stat(pos[0], &sa) == 0 && S_ISDIR(sa.st_mode);
    int isdb = stat(pos[1], &sb) == 0 && S_ISDIR(sb.st_mode);

    if (isda || isdb) {
        char pa[1024], pb[1024];
        const char *fa = pos[0], *fb = pos[1];
        if (isda && !isdb) {
            const char *base = strrchr(pos[1], '/');
            base = base ? base + 1 : pos[1];
            snprintf(pa, sizeof pa, "%s/%s", pos[0], base);
            fa = pa;
        } else if (isdb && !isda) {
            const char *base = strrchr(pos[0], '/');
            base = base ? base + 1 : pos[0];
            snprintf(pb, sizeof pb, "%s/%s", pos[1], base);
            fb = pb;
        }
        if (isda && isdb) return diff_tree(pos[0], pos[1], &o);
        return diff_one(fa, fb, &o);
    }

    int rc = diff_one(pos[0], pos[1], &o);
    return rc;
}
