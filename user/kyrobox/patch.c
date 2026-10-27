#include "common.h"

typedef struct {
    char type; /* ' ' context, '-' delete, '+' insert */
    char *text;
} hline_t;

typedef struct {
    int a_start, a_count, b_start, b_count; /* 0 count means "position, no lines" */
    hline_t *lines;
    int nlines, cap;
} hunk_t;

typedef struct {
    char *oldfile, *newfile;
    hunk_t *hunks;
    int nhunks, cap;
} filepatch_t;

typedef struct {
    char **lines;
    int n;
} lines_t;

static void lines_free(lines_t *l) {
    for (int i = 0; i < l->n; i++) free(l->lines[i]);
    free(l->lines);
}

static lines_t read_all_lines(FILE *f) {
    lines_t l = { 0 };
    int cap = 0;
    char *line = NULL;
    size_t bcap = 0;
    ssize_t len;
    while ((len = getline(&line, &bcap, f)) >= 0) {
        if (len > 0 && line[(size_t) len - 1] == '\n') line[--len] = 0;
        if (l.n >= cap) {
            cap = cap ? cap * 2 : 256;
            l.lines = realloc(l.lines, (size_t) cap * sizeof(char *));
        }
        l.lines[l.n++] = strdup(line);
    }
    free(line);
    return l;
}

static hunk_t *fp_new_hunk(filepatch_t *fp) {
    if (fp->nhunks >= fp->cap) {
        fp->cap = fp->cap ? fp->cap * 2 : 8;
        fp->hunks = realloc(fp->hunks, (size_t) fp->cap * sizeof(hunk_t));
    }
    hunk_t *h = &fp->hunks[fp->nhunks++];
    memset(h, 0, sizeof(*h));
    return h;
}

static void hunk_push(hunk_t *h, char type, const char *text) {
    if (h->nlines >= h->cap) {
        h->cap = h->cap ? h->cap * 2 : 32;
        h->lines = realloc(h->lines, (size_t) h->cap * sizeof(hline_t));
    }
    h->lines[h->nlines].type = type;
    h->lines[h->nlines].text = strdup(text);
    h->nlines++;
}

static const char *strip_p(const char *path, int p) {
    while (p-- > 0) {
        const char *slash = strchr(path, '/');
        if (!slash) break;
        path = slash + 1;
    }
    return path;
}

static char *header_name(const char *hdr) {
    static char buf[512];
    snprintf(buf, sizeof buf, "%s", hdr);
    char *tab = strchr(buf, '\t');
    if (tab) *tab = 0;
    return buf;
}

static int parse_unified(lines_t *p, int *pi, filepatch_t *fp) {
    int i = *pi;
    fp->oldfile = strdup(header_name(p->lines[i] + 4));
    i++;
    fp->newfile = strdup(header_name(p->lines[i] + 4));
    i++;

    while (i < p->n && strncmp(p->lines[i], "@@ ", 3) == 0) {
        hunk_t *h = fp_new_hunk(fp);
        int got = sscanf(p->lines[i], "@@ -%d,%d +%d,%d @@", &h->a_start, &h->a_count, &h->b_start,
                         &h->b_count);
        if (got < 4) {
            h->a_count = h->b_count = 1;
            sscanf(p->lines[i], "@@ -%d +%d @@", &h->a_start, &h->b_start);
        }
        i++;
        while (i < p->n &&
               (p->lines[i][0] == ' ' || p->lines[i][0] == '-' || p->lines[i][0] == '+')) {
            hunk_push(h, p->lines[i][0], p->lines[i] + 1);
            i++;
        }
        while (i < p->n && p->lines[i][0] == '\\') i++;
    }
    *pi = i;
    return 0;
}

static int parse_context(lines_t *p, int *pi, filepatch_t *fp) {
    int i = *pi;
    fp->oldfile = strdup(header_name(p->lines[i] + 4));
    i++;
    fp->newfile = strdup(header_name(p->lines[i] + 4));
    i++;

    while (i < p->n && strncmp(p->lines[i], "***************", 15) == 0) {
        i++;
        if (i >= p->n || strncmp(p->lines[i], "*** ", 4) != 0) break;
        hunk_t *h = fp_new_hunk(fp);
        h->a_count = 1;
        sscanf(p->lines[i], "*** %d,%d", &h->a_start, &h->a_count);
        if (strchr(p->lines[i], ',') == NULL) {
            int one;
            if (sscanf(p->lines[i], "*** %d", &one) == 1) {
                h->a_start = one;
                h->a_count = 1;
            }
        }
        i++;

        typedef struct {
            char mark;
            char *text;
        } raw_t;
        raw_t *old_raw = NULL;
        int n_old = 0, cap_old = 0;
        while (i < p->n && strncmp(p->lines[i], "--- ", 4) != 0) {
            if (n_old >= cap_old) {
                cap_old = cap_old ? cap_old * 2 : 16;
                old_raw = realloc(old_raw, (size_t) cap_old * sizeof(raw_t));
            }
            old_raw[n_old].mark = p->lines[i][0];
            old_raw[n_old].text = p->lines[i] + 2;
            n_old++;
            i++;
        }

        h->b_count = 1;
        if (i < p->n) {
            sscanf(p->lines[i], "--- %d,%d", &h->b_start, &h->b_count);
            if (strchr(p->lines[i], ',') == NULL) {
                int one;
                if (sscanf(p->lines[i], "--- %d", &one) == 1) {
                    h->b_start = one;
                    h->b_count = 1;
                }
            }
            i++;
        }

        raw_t *new_raw = NULL;
        int n_new = 0, cap_new = 0;
        while (i < p->n && p->lines[i][0] != '*' && p->lines[i][0] != '-' &&
               strncmp(p->lines[i], "***************", 3) != 0 &&
               (p->lines[i][0] == ' ' || p->lines[i][0] == '+' || p->lines[i][0] == '!')) {
            if (n_new >= cap_new) {
                cap_new = cap_new ? cap_new * 2 : 16;
                new_raw = realloc(new_raw, (size_t) cap_new * sizeof(raw_t));
            }
            new_raw[n_new].mark = p->lines[i][0];
            new_raw[n_new].text = p->lines[i] + 2;
            n_new++;
            i++;
        }

        int oi = 0, ni = 0;
        while (oi < n_old || ni < n_new) {
            while (oi < n_old && old_raw[oi].mark != ' ') {
                hunk_push(h, '-', old_raw[oi].text);
                oi++;
            }
            while (ni < n_new && new_raw[ni].mark != ' ') {
                hunk_push(h, '+', new_raw[ni].text);
                ni++;
            }
            if (oi < n_old && ni < n_new) {
                hunk_push(h, ' ', old_raw[oi].text);
                oi++;
                ni++;
            } else if (oi < n_old) {
                oi++;
            } else if (ni < n_new) {
                ni++;
            } else {
                break;
            }
        }
        free(old_raw);
        free(new_raw);
    }
    *pi = i;
    return 0;
}

static int looks_like_normal_hdr(const char *s) {
    if (!isdigit((unsigned char) s[0])) return 0;
    const char *p = s;
    while (isdigit((unsigned char) *p)) p++;
    if (*p == ',') {
        p++;
        if (!isdigit((unsigned char) *p)) return 0;
        while (isdigit((unsigned char) *p)) p++;
    }
    if (*p != 'a' && *p != 'c' && *p != 'd') return 0;
    p++;
    if (!isdigit((unsigned char) *p)) return 0;
    while (isdigit((unsigned char) *p)) p++;
    if (*p == ',') {
        p++;
        if (!isdigit((unsigned char) *p)) return 0;
        while (isdigit((unsigned char) *p)) p++;
    }
    return *p == 0;
}

static int parse_normal(lines_t *p, int *pi, filepatch_t *fp) {
    int i = *pi;
    fp->oldfile = NULL;
    fp->newfile = NULL;

    while (i < p->n && looks_like_normal_hdr(p->lines[i])) {
        char cmd = 0;
        int as, ac = 1, bs, bc = 1;
        const char *s = p->lines[i];
        char *comma1;
        as = (int) strtol(s, &comma1, 10);
        if (*comma1 == ',') { ac = (int) strtol(comma1 + 1, &comma1, 10) - as + 1; }
        cmd = *comma1;
        bs = (int) strtol(comma1 + 1, &comma1, 10);
        if (*comma1 == ',') bc = (int) strtol(comma1 + 1, &comma1, 10) - bs + 1;
        i++;

        hunk_t *h = fp_new_hunk(fp);
        if (cmd == 'a') {
            h->a_start = as;
            h->a_count = 0;
            h->b_start = bs;
            h->b_count = bc;
        } else if (cmd == 'd') {
            h->a_start = as;
            h->a_count = ac;
            h->b_start = bs;
            h->b_count = 0;
        } else {
            h->a_start = as;
            h->a_count = ac;
            h->b_start = bs;
            h->b_count = bc;
        }

        if (cmd == 'c' || cmd == 'd') {
            while (i < p->n && strncmp(p->lines[i], "< ", 2) == 0) {
                hunk_push(h, '-', p->lines[i] + 2);
                i++;
            }
        }
        if (cmd == 'c' && i < p->n && strcmp(p->lines[i], "---") == 0) i++;
        if (cmd == 'c' || cmd == 'a') {
            while (i < p->n && strncmp(p->lines[i], "> ", 2) == 0) {
                hunk_push(h, '+', p->lines[i] + 2);
                i++;
            }
        }
    }
    *pi = i;
    return 0;
}

static void reverse_patch(filepatch_t *fp) {
    char *t = fp->oldfile;
    fp->oldfile = fp->newfile;
    fp->newfile = t;
    for (int h = 0; h < fp->nhunks; h++) {
        hunk_t *hu = &fp->hunks[h];
        int ts = hu->a_start, tc = hu->a_count;
        hu->a_start = hu->b_start;
        hu->a_count = hu->b_count;
        hu->b_start = ts;
        hu->b_count = tc;
        for (int k = 0; k < hu->nlines; k++) {
            if (hu->lines[k].type == '-')
                hu->lines[k].type = '+';
            else if (hu->lines[k].type == '+')
                hu->lines[k].type = '-';
        }
    }
}

typedef struct {
    int p;
    const char *target;
    const char *input;
    const char *output;
    const char *directory;
    const char *reject_file;
    int reverse;
    int backup;
    int remove_empty;
    int force;
    int ignore_ws;
    int quiet;
    int dry_run;
    int fuzz;
    int skip_applied;
} popts_t;

static int text_eq(const char *a, const char *b, int loose) {
    if (!loose) return strcmp(a, b) == 0;
    while (*a && *b) {
        while (isspace((unsigned char) *a)) a++;
        while (isspace((unsigned char) *b)) b++;
        if (*a == 0 || *b == 0) break;
        if (*a != *b) return 0;
        a++;
        b++;
    }
    while (isspace((unsigned char) *a)) a++;
    while (isspace((unsigned char) *b)) b++;
    return *a == 0 && *b == 0;
}

static int block_matches(char **src, int nsrc, int pos, hline_t *lines, int nlines, char keep_type,
                         int loose, int skip_lead, int skip_tail) {
    int si = pos;
    int total = 0;
    for (int k = 0; k < nlines; k++)
        if (lines[k].type == ' ' || lines[k].type == keep_type) total++;
    int idx = 0;
    for (int k = 0; k < nlines; k++) {
        if (lines[k].type != ' ' && lines[k].type != keep_type) continue;
        int fuzzed = (idx < skip_lead) || (idx >= total - skip_tail);
        if (!fuzzed) {
            if (si >= nsrc) return 0;
            if (!text_eq(src[si], lines[k].text, loose)) return 0;
        }
        si++;
        idx++;
    }
    return 1;
}

static int search_pos(char **src, int nsrc, int guess, hline_t *lines, int nlines, char keep_type,
                      int loose, int fuzz, int *used_fuzz) {
    int total = 0;
    for (int k = 0; k < nlines; k++)
        if (lines[k].type == ' ' || lines[k].type == keep_type) total++;

    if (guess < 0) guess = 0;
    int window = nsrc + 1;
    for (int d = 0; d <= window; d++) {
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            if (d == 0 && sgn == 1) continue;
            int pos = guess + sgn * d;
            if (pos < 0 || pos + total > nsrc) continue;
            if (block_matches(src, nsrc, pos, lines, nlines, keep_type, loose, 0, 0)) {
                *used_fuzz = 0;
                return pos;
            }
        }
        if (d == 0) continue;
    }
    if (fuzz > 0) {
        for (int f = 1; f <= fuzz; f++) {
            for (int d = 0; d <= window; d++) {
                for (int sgn = -1; sgn <= 1; sgn += 2) {
                    if (d == 0 && sgn == 1) continue;
                    int pos = guess + sgn * d;
                    if (pos < 0 || pos + total > nsrc) continue;
                    if (block_matches(src, nsrc, pos, lines, nlines, keep_type, loose, f, f)) {
                        *used_fuzz = f;
                        return pos;
                    }
                }
            }
        }
    }
    return -1;
}

static int apply_file(filepatch_t *fp, popts_t *o) {
    const char *filename = o->target ? o->target : strip_p(fp->newfile, o->p);
    if (o->reverse && !o->target) filename = strip_p(fp->oldfile, o->p);

    FILE *orig = fopen(filename, "r");
    lines_t src = { 0 };
    if (orig) {
        src = read_all_lines(orig);
        fclose(orig);
    }

    lines_t out = { 0 };
    int outcap = 0;
    int si = 0;
    int running = 0;
    int any_fail = 0;
    lines_t rej = { 0 };
    int rejcap = 0;

    for (int h = 0; h < fp->nhunks; h++) {
        hunk_t *hu = &fp->hunks[h];
        int old_total = 0;
        for (int k = 0; k < hu->nlines; k++)
            if (hu->lines[k].type == ' ' || hu->lines[k].type == '-') old_total++;

        int guess = (hu->a_count > 0 ? hu->a_start - 1 : hu->a_start) + running;
        int used_fuzz = 0;
        int pos;
        if (o->force) {
            pos = guess;
            if (pos < 0) pos = 0;
            if (pos > src.n) pos = src.n;
        } else {
            pos = search_pos(src.lines, src.n, guess, hu->lines, hu->nlines, '-', o->ignore_ws,
                             o->fuzz, &used_fuzz);
        }

        if (pos < 0) {
            any_fail = 1;
            if (!o->quiet) fprintf(stderr, "Hunk #%d FAILED at %d.\n", h + 1, guess + 1);
            if (rej.n >= rejcap) {
                rejcap = rejcap ? rejcap * 2 : 8;
                rej.lines = realloc(rej.lines, (size_t) rejcap * sizeof(char *));
            }
            char hdr[64];
            snprintf(hdr, sizeof hdr, "@@ -%d,%d +%d,%d @@", hu->a_start, hu->a_count, hu->b_start,
                     hu->b_count);
            rej.lines[rej.n++] = strdup(hdr);
            for (int k = 0; k < hu->nlines; k++) {
                char buf[1024];
                snprintf(buf, sizeof buf, "%c%s", hu->lines[k].type, hu->lines[k].text);
                if (rej.n >= rejcap) {
                    rejcap *= 2;
                    rej.lines = realloc(rej.lines, (size_t) rejcap * sizeof(char *));
                }
                rej.lines[rej.n++] = strdup(buf);
            }
            continue;
        }

        if (!o->quiet && used_fuzz)
            fprintf(stderr, "Hunk #%d succeeded at %d with fuzz %d.\n", h + 1, pos + 1, used_fuzz);

        while (si < pos && si < src.n) {
            if (out.n >= outcap) {
                outcap = outcap ? outcap * 2 : 256;
                out.lines = realloc(out.lines, (size_t) outcap * sizeof(char *));
            }
            out.lines[out.n++] = strdup(src.lines[si++]);
        }

        for (int k = 0; k < hu->nlines; k++) {
            if (hu->lines[k].type == ' ') {
                if (out.n >= outcap) {
                    outcap = outcap ? outcap * 2 : 256;
                    out.lines = realloc(out.lines, (size_t) outcap * sizeof(char *));
                }
                out.lines[out.n++] = strdup(hu->lines[k].text);
                si++;
            } else if (hu->lines[k].type == '-') {
                si++;
            } else {
                if (out.n >= outcap) {
                    outcap = outcap ? outcap * 2 : 256;
                    out.lines = realloc(out.lines, (size_t) outcap * sizeof(char *));
                }
                out.lines[out.n++] = strdup(hu->lines[k].text);
            }
        }
        running += (hu->b_count - hu->a_count);
        (void) old_total;
    }

    while (si < src.n) {
        if (out.n >= outcap) {
            outcap = outcap ? outcap * 2 : 256;
            out.lines = realloc(out.lines, (size_t) outcap * sizeof(char *));
        }
        out.lines[out.n++] = strdup(src.lines[si++]);
    }

    if (!o->dry_run) {
        if (o->backup && src.lines) {
            char bak[600];
            snprintf(bak, sizeof bak, "%s.orig", filename);
            FILE *bf = fopen(bak, "w");
            if (bf) {
                for (int k = 0; k < src.n; k++) fprintf(bf, "%s\n", src.lines[k]);
                fclose(bf);
            }
        }

        const char *outname = o->output ? o->output : filename;
        if (o->remove_empty && out.n == 0) {
            remove(outname);
        } else {
            FILE *w = fopen(outname, "w");
            if (!w) {
                kx_warn(outname);
            } else {
                for (int k = 0; k < out.n; k++) fprintf(w, "%s\n", out.lines[k]);
                fclose(w);
            }
        }

        if (rej.n > 0) {
            char rejname[600];
            snprintf(rejname, sizeof rejname, "%s", o->reject_file ? o->reject_file : "");
            if (!o->reject_file) snprintf(rejname, sizeof rejname, "%s.rej", filename);
            FILE *rf = fopen(rejname, "w");
            if (rf) {
                for (int k = 0; k < rej.n; k++) fprintf(rf, "%s\n", rej.lines[k]);
                fclose(rf);
            }
        }
    }

    if (!o->quiet) printf("patching file %s\n", filename);

    lines_free(&src);
    lines_free(&out);
    lines_free(&rej);
    return any_fail ? 1 : 0;
}

static void free_filepatch(filepatch_t *fp) {
    free(fp->oldfile);
    free(fp->newfile);
    for (int h = 0; h < fp->nhunks; h++) {
        for (int k = 0; k < fp->hunks[h].nlines; k++) free(fp->hunks[h].lines[k].text);
        free(fp->hunks[h].lines);
    }
    free(fp->hunks);
}

static void usage(void) {
    kx_die("usage: patch [-p N] [-R] [-b] [-f] [-l] [-s] [-E] [-N] [-F N] [-i FILE] [-o FILE] "
           "[-d DIR] [-r FILE] [--dry-run] [FILE]");
}

int main(int argc, char **argv) {
    kx_prog = "patch";
    popts_t o = { 0 };

    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (strncmp(a, "-p", 2) == 0 && a[2]) {
            o.p = atoi(a + 2);
        } else if (strcmp(a, "-p") == 0 && i + 1 < argc) {
            o.p = atoi(argv[++i]);
        } else if (strncmp(a, "--strip=", 8) == 0) {
            o.p = atoi(a + 8);
        } else if (strcmp(a, "-i") == 0 && i + 1 < argc) {
            o.input = argv[++i];
        } else if (strncmp(a, "--input=", 8) == 0) {
            o.input = a + 8;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            o.output = argv[++i];
        } else if (strncmp(a, "--output=", 9) == 0) {
            o.output = a + 9;
        } else if (strcmp(a, "-d") == 0 && i + 1 < argc) {
            o.directory = argv[++i];
        } else if (strncmp(a, "--directory=", 12) == 0) {
            o.directory = a + 12;
        } else if (strcmp(a, "-r") == 0 && i + 1 < argc) {
            o.reject_file = argv[++i];
        } else if (strncmp(a, "--reject-file=", 14) == 0) {
            o.reject_file = a + 14;
        } else if (strncmp(a, "-F", 2) == 0 && a[2]) {
            o.fuzz = atoi(a + 2);
        } else if (strcmp(a, "-F") == 0 && i + 1 < argc) {
            o.fuzz = atoi(argv[++i]);
        } else if (strncmp(a, "--fuzz=", 7) == 0) {
            o.fuzz = atoi(a + 7);
        } else if (strcmp(a, "-R") == 0 || strcmp(a, "--reverse") == 0) {
            o.reverse = 1;
        } else if (strcmp(a, "-b") == 0 || strcmp(a, "--backup") == 0) {
            o.backup = 1;
        } else if (strcmp(a, "-E") == 0 || strcmp(a, "--remove-empty-files") == 0) {
            o.remove_empty = 1;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--force") == 0) {
            o.force = 1;
        } else if (strcmp(a, "-l") == 0 || strcmp(a, "--ignore-whitespace") == 0) {
            o.ignore_ws = 1;
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--quiet") == 0 ||
                   strcmp(a, "--silent") == 0) {
            o.quiet = 1;
        } else if (strcmp(a, "-N") == 0 || strcmp(a, "--forward") == 0) {
            o.skip_applied = 1;
        } else if (strcmp(a, "--dry-run") == 0) {
            o.dry_run = 1;
        } else if (strcmp(a, "-t") == 0 || strcmp(a, "-u") == 0 || strcmp(a, "-c") == 0 ||
                   strcmp(a, "-n") == 0) {
        } else if (a[0] == '-' && a[1] != 0) {
            usage();
        } else {
            o.target = a;
        }
    }

    if (o.directory && chdir(o.directory) != 0) kx_die("cannot chdir");

    FILE *pf = o.input ? fopen(o.input, "r") : stdin;
    if (!pf) kx_die("cannot open patch input");
    lines_t patch = read_all_lines(pf);
    if (pf != stdin) fclose(pf);

    int pi = 0;
    int overall_fail = 0;
    int applied_any = 0;

    while (pi < patch.n) {
        while (pi < patch.n && strncmp(patch.lines[pi], "--- ", 4) != 0 &&
               strncmp(patch.lines[pi], "*** ", 4) != 0 && !looks_like_normal_hdr(patch.lines[pi]))
            pi++;
        if (pi >= patch.n) break;

        filepatch_t fp = { 0 };
        if (strncmp(patch.lines[pi], "--- ", 4) == 0 && pi + 1 < patch.n &&
            strncmp(patch.lines[pi + 1], "+++ ", 4) == 0) {
            parse_unified(&patch, &pi, &fp);
        } else if (strncmp(patch.lines[pi], "*** ", 4) == 0 && pi + 1 < patch.n &&
                   strncmp(patch.lines[pi + 1], "--- ", 4) == 0) {
            parse_context(&patch, &pi, &fp);
        } else if (looks_like_normal_hdr(patch.lines[pi])) {
            if (!o.target) {
                pi++;
                continue;
            }
            parse_normal(&patch, &pi, &fp);
        } else {
            pi++;
            continue;
        }

        if (o.reverse) reverse_patch(&fp);
        if (fp.nhunks > 0) {
            int rc = apply_file(&fp, &o);
            if (rc) overall_fail = 1;
            applied_any = 1;
        }
        free_filepatch(&fp);
    }

    lines_free(&patch);
    if (!applied_any) kx_die("no patch data found");
    return overall_fail ? 1 : 0;
}
