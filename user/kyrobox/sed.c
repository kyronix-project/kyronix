#include "common.h"
#include <regex.h>

typedef enum { ADDR_NONE, ADDR_LINE, ADDR_LAST, ADDR_REGEX } addrkind_t;

typedef struct {
    addrkind_t kind;
    long line;
    regex_t re;
} addr_t;

typedef struct {
    addr_t a1, a2;
    bool have_a2;
    bool in_range;
    char cmd;
    regex_t sre;
    char *repl;
    bool s_global;
    bool s_icase;
    bool s_print;
} cmd_t;

static cmd_t *cmds;
static int ncmds, capcmds;

static void buf_append(char **buf, size_t *cap, size_t *len, const char *data, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap ? *cap * 2 : 64;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
}

static char *extract_delim(const char **pp, char delim) {
    const char *p = *pp;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    while (*p && *p != delim) {
        if (*p == '\\' && p[1] == delim) {
            buf_append(&buf, &cap, &len, &delim, 1);
            p += 2;
        } else if (*p == '\\' && p[1]) {
            buf_append(&buf, &cap, &len, p, 2);
            p += 2;
        } else {
            buf_append(&buf, &cap, &len, p, 1);
            p++;
        }
    }
    if (*p == delim) p++;
    if (!buf) {
        buf = malloc(1);
        buf[0] = 0;
    }
    *pp = p;
    return buf;
}

static void push_cmd(cmd_t c) {
    if (ncmds == capcmds) {
        capcmds = capcmds ? capcmds * 2 : 16;
        cmds = realloc(cmds, (size_t) capcmds * sizeof(*cmds));
    }
    cmds[ncmds++] = c;
}

static void skip_ws(const char **pp) {
    while (**pp == ' ' || **pp == '\t') (*pp)++;
}

static void skip_sep(const char **pp) {
    while (**pp == ';' || **pp == '\n' || **pp == ' ' || **pp == '\t') (*pp)++;
}

static bool parse_addr(const char **pp, addr_t *a) {
    const char *p = *pp;
    if (*p == '$') {
        a->kind = ADDR_LAST;
        *pp = p + 1;
        return true;
    }
    if (isdigit((unsigned char) *p)) {
        char *end;
        long v = strtol(p, &end, 10);
        a->kind = ADDR_LINE;
        a->line = v;
        *pp = end;
        return true;
    }
    if (*p == '/') {
        p++;
        char *pat = extract_delim(&p, '/');
        if (regcomp(&a->re, pat, REG_EXTENDED) != 0) kx_die("bad address regex");
        free(pat);
        a->kind = ADDR_REGEX;
        *pp = p;
        return true;
    }
    return false;
}

static void parse_script(const char *script) {
    const char *p = script;
    for (;;) {
        skip_sep(&p);
        if (!*p) break;
        cmd_t c;
        memset(&c, 0, sizeof(c));
        c.a1.kind = ADDR_NONE;
        c.a2.kind = ADDR_NONE;
        if (parse_addr(&p, &c.a1)) {
            skip_ws(&p);
            if (*p == ',') {
                p++;
                skip_ws(&p);
                if (!parse_addr(&p, &c.a2)) kx_die("expected address after ','");
                c.have_a2 = true;
            }
        }
        skip_ws(&p);
        if (!*p) kx_die("missing command");
        char cmdch = *p++;
        c.cmd = cmdch;
        if (cmdch == 's') {
            if (!*p) kx_die("bad s command");
            char delim = *p++;
            char *pat = extract_delim(&p, delim);
            char *repl = extract_delim(&p, delim);
            while (*p == 'g' || *p == 'i' || *p == 'p') {
                if (*p == 'g')
                    c.s_global = true;
                else if (*p == 'i')
                    c.s_icase = true;
                else
                    c.s_print = true;
                p++;
            }
            int flags = REG_EXTENDED | (c.s_icase ? REG_ICASE : 0);
            if (regcomp(&c.sre, pat, flags) != 0) kx_die("bad regex in s command");
            free(pat);
            c.repl = repl;
        } else if (cmdch != 'p' && cmdch != 'd') {
            kx_die("unsupported sed command");
        }
        push_cmd(c);
    }
}

static bool addr_match_single(addr_t *a, const char *line, long lineno, long total) {
    switch (a->kind) {
    case ADDR_LINE:
        return lineno == a->line;
    case ADDR_LAST:
        return lineno == total;
    case ADDR_REGEX:
        return regexec(&a->re, line, 0, NULL, 0) == 0;
    default:
        return false;
    }
}

static bool cmd_applies(cmd_t *c, const char *line, long lineno, long total) {
    if (c->a1.kind == ADDR_NONE) return true;
    if (!c->have_a2) return addr_match_single(&c->a1, line, lineno, total);
    if (!c->in_range) {
        if (!addr_match_single(&c->a1, line, lineno, total)) return false;
        c->in_range = true;
        if (c->a2.kind == ADDR_LINE && c->a2.line <= lineno)
            c->in_range = false;
        else if (c->a2.kind == ADDR_REGEX && addr_match_single(&c->a2, line, lineno, total))
            c->in_range = false;
        return true;
    }
    if (addr_match_single(&c->a2, line, lineno, total)) c->in_range = false;
    return true;
}

static char *do_subst(cmd_t *c, const char *line, bool *changed) {
    regmatch_t m[10];
    char *out = NULL;
    size_t cap = 0, len = 0;
    *changed = false;
    const char *p = line;
    for (;;) {
        int eflags = (p != line) ? REG_NOTBOL : 0;
        if (regexec(&c->sre, p, 10, m, eflags) != 0) break;
        *changed = true;
        buf_append(&out, &cap, &len, p, (size_t) m[0].rm_so);
        for (const char *r = c->repl; *r; r++) {
            if (*r == '\\' && r[1] >= '0' && r[1] <= '9') {
                int gi = r[1] - '0';
                if (m[gi].rm_so >= 0)
                    buf_append(&out, &cap, &len, p + m[gi].rm_so,
                               (size_t) (m[gi].rm_eo - m[gi].rm_so));
                r++;
            } else if (*r == '\\' && r[1]) {
                buf_append(&out, &cap, &len, r + 1, 1);
                r++;
            } else if (*r == '&') {
                buf_append(&out, &cap, &len, p + m[0].rm_so, (size_t) (m[0].rm_eo - m[0].rm_so));
            } else {
                buf_append(&out, &cap, &len, r, 1);
            }
        }
        const char *matchend = p + m[0].rm_eo;
        if (m[0].rm_eo == m[0].rm_so) {
            if (*matchend) buf_append(&out, &cap, &len, matchend, 1);
            p = *matchend ? matchend + 1 : matchend;
        } else {
            p = matchend;
        }
        if (!c->s_global || !*p) break;
    }
    buf_append(&out, &cap, &len, p, strlen(p));
    if (!out) out = strdup("");
    return out;
}

static char **read_lines(FILE *f, long *out_n) {
    char **lines = NULL;
    long n = 0, cap = 0;
    char *buf = NULL;
    size_t bufcap = 0;
    ssize_t len;
    while ((len = getline(&buf, &bufcap, f)) >= 0) {
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = 0;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            lines = realloc(lines, (size_t) cap * sizeof(*lines));
        }
        lines[n++] = strdup(buf);
    }
    free(buf);
    *out_n = n;
    return lines;
}

static void process_lines(char **lines, long n, FILE *out, bool suppress) {
    for (int i = 0; i < ncmds; i++) cmds[i].in_range = false;
    for (long ln = 0; ln < n; ln++) {
        char *line = lines[ln];
        bool deleted = false;
        for (int ci = 0; ci < ncmds; ci++) {
            cmd_t *c = &cmds[ci];
            if (!cmd_applies(c, line, ln + 1, n)) continue;
            if (c->cmd == 'd') {
                deleted = true;
                break;
            } else if (c->cmd == 'p') {
                fputs(line, out);
                fputc('\n', out);
            } else {
                bool changed;
                char *newline = do_subst(c, line, &changed);
                if (changed) {
                    free(lines[ln]);
                    lines[ln] = newline;
                    line = newline;
                    if (c->s_print) {
                        fputs(line, out);
                        fputc('\n', out);
                    }
                } else {
                    free(newline);
                }
            }
        }
        if (!deleted && !suppress) {
            fputs(line, out);
            fputc('\n', out);
        }
    }
}

int main(int argc, char **argv) {
    kx_prog = "sed";
    bool opt_i = false, suppress = false;
    char *i_suffix = NULL;
    char *script_buf = NULL;
    size_t script_cap = 0, script_len = 0;
    bool have_script = false;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        }
        if (strncmp(a, "-i", 2) == 0) {
            opt_i = true;
            if (a[2]) i_suffix = strdup(a + 2);
            continue;
        }
        if (strncmp(a, "-e", 2) == 0) {
            const char *val = a[2] ? a + 2 : (++i < argc ? argv[i] : NULL);
            if (!val) kx_die("usage: sed [-n] [-i[SUFFIX]] [-e SCRIPT]... [SCRIPT] [FILE...]");
            if (have_script) buf_append(&script_buf, &script_cap, &script_len, "\n", 1);
            buf_append(&script_buf, &script_cap, &script_len, val, strlen(val));
            have_script = true;
            continue;
        }
        if (strcmp(a, "-n") == 0) {
            suppress = true;
            continue;
        }
        kx_die("usage: sed [-n] [-i[SUFFIX]] [-e SCRIPT]... [SCRIPT] [FILE...]");
    }
    if (!have_script) {
        if (i >= argc) kx_die("usage: sed [-n] [-i[SUFFIX]] [-e SCRIPT]... [SCRIPT] [FILE...]");
        const char *val = argv[i++];
        buf_append(&script_buf, &script_cap, &script_len, val, strlen(val));
        have_script = true;
    }
    if (!script_buf) script_buf = strdup("");
    parse_script(script_buf);

    int rc = 0;
    if (i == argc) {
        if (opt_i) kx_die("-i requires file arguments");
        long n;
        char **lines = read_lines(stdin, &n);
        process_lines(lines, n, stdout, suppress);
        for (long k = 0; k < n; k++) free(lines[k]);
        free(lines);
    } else {
        for (; i < argc; i++) {
            const char *path = argv[i];
            FILE *f = fopen(path, "r");
            if (!f) {
                kx_warn(path);
                rc = 1;
                continue;
            }
            long n;
            char **lines = read_lines(f, &n);
            fclose(f);
            if (opt_i) {
                if (i_suffix) {
                    char backup[PATH_MAX];
                    snprintf(backup, sizeof(backup), "%s%s", path, i_suffix);
                    kx_copy_file(path, backup);
                }
                FILE *out = fopen(path, "w");
                if (!out) {
                    kx_warn(path);
                    rc = 1;
                } else {
                    process_lines(lines, n, out, suppress);
                    fclose(out);
                }
            } else {
                process_lines(lines, n, stdout, suppress);
            }
            for (long k = 0; k < n; k++) free(lines[k]);
            free(lines);
        }
    }
    return rc;
}
