#include "common.h"
#include <sys/socket.h>

typedef struct {
    int argc;
    char **argv;
    int pos;
    bool error;
} parser_t;

static bool integer(const char *s, long long *value) {
    char *end;
    errno = 0;
    long long n = strtoll(s, &end, 10);
    if (end == s || *end || errno == ERANGE) return false;
    *value = n;
    return true;
}

static bool unary(const char *op, const char *arg) {
    struct stat st;
    struct stat lst;
    if (strcmp(op, "-n") == 0) return *arg != '\0';
    if (strcmp(op, "-z") == 0) return *arg == '\0';
    if (strcmp(op, "-t") == 0) {
        long long fd;
        return integer(arg, &fd) && fd >= 0 && fd <= INT_MAX && isatty((int) fd);
    }
    bool have = stat(arg, &st) == 0;
    bool lhave = lstat(arg, &lst) == 0;
    if (strcmp(op, "-a") == 0 || strcmp(op, "-e") == 0) return have;
    if (strcmp(op, "-b") == 0) return have && S_ISBLK(st.st_mode);
    if (strcmp(op, "-c") == 0) return have && S_ISCHR(st.st_mode);
    if (strcmp(op, "-d") == 0) return have && S_ISDIR(st.st_mode);
    if (strcmp(op, "-f") == 0) return have && S_ISREG(st.st_mode);
    if (strcmp(op, "-g") == 0) return have && (st.st_mode & S_ISGID);
    if (strcmp(op, "-h") == 0 || strcmp(op, "-L") == 0)
        return lhave && S_ISLNK(lst.st_mode);
    if (strcmp(op, "-k") == 0) return have && (st.st_mode & S_ISVTX);
    if (strcmp(op, "-p") == 0) return have && S_ISFIFO(st.st_mode);
    if (strcmp(op, "-r") == 0) return access(arg, R_OK) == 0;
    if (strcmp(op, "-s") == 0) return have && st.st_size > 0;
    if (strcmp(op, "-S") == 0) return have && S_ISSOCK(st.st_mode);
    if (strcmp(op, "-u") == 0) return have && (st.st_mode & S_ISUID);
    if (strcmp(op, "-w") == 0) return access(arg, W_OK) == 0;
    if (strcmp(op, "-x") == 0) return access(arg, X_OK) == 0;
    if (strcmp(op, "-G") == 0) return have && st.st_gid == getegid();
    if (strcmp(op, "-O") == 0) return have && st.st_uid == geteuid();
    return false;
}

static bool is_unary(const char *s) {
    static const char *const ops[] = {
        "-a", "-b", "-c", "-d", "-e", "-f", "-g", "-G", "-h", "-k", "-L",
        "-n", "-O", "-p", "-r", "-s", "-S", "-t", "-u", "-w", "-x", "-z"
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (strcmp(s, ops[i]) == 0) return true;
    return false;
}

static bool is_binary(const char *s) {
    static const char *const ops[] = {
        "=", "==", "!=", "<", ">", "-eq", "-ne", "-gt", "-ge", "-lt", "-le", "-ef", "-nt", "-ot"
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (strcmp(s, ops[i]) == 0) return true;
    return false;
}

static bool binary(parser_t *p, const char *left, const char *op, const char *right) {
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) return strcmp(left, right) == 0;
    if (strcmp(op, "!=") == 0) return strcmp(left, right) != 0;
    if (strcmp(op, "<") == 0) return strcmp(left, right) < 0;
    if (strcmp(op, ">") == 0) return strcmp(left, right) > 0;
    if (op[0] == '-' && strchr("englt", op[1]) && op[2]) {
        long long a, b;
        if (!integer(left, &a) || !integer(right, &b)) {
            p->error = true;
            return false;
        }
        if (strcmp(op, "-eq") == 0) return a == b;
        if (strcmp(op, "-ne") == 0) return a != b;
        if (strcmp(op, "-gt") == 0) return a > b;
        if (strcmp(op, "-ge") == 0) return a >= b;
        if (strcmp(op, "-lt") == 0) return a < b;
        if (strcmp(op, "-le") == 0) return a <= b;
    }
    struct stat a, b;
    if (stat(left, &a) < 0 || stat(right, &b) < 0) return false;
    if (strcmp(op, "-ef") == 0) return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
    if (strcmp(op, "-nt") == 0) return a.st_mtime > b.st_mtime;
    if (strcmp(op, "-ot") == 0) return a.st_mtime < b.st_mtime;
    return false;
}

static bool parse_or(parser_t *p);

static bool parse_primary(parser_t *p) {
    if (p->pos >= p->argc) {
        p->error = true;
        return false;
    }
    const char *token = p->argv[p->pos++];
    if (strcmp(token, "(") == 0) {
        bool result = parse_or(p);
        if (p->pos >= p->argc || strcmp(p->argv[p->pos], ")") != 0)
            p->error = true;
        else
            p->pos++;
        return result;
    }
    if (is_unary(token)) {
        if (p->pos >= p->argc) {
            p->error = true;
            return false;
        }
        return unary(token, p->argv[p->pos++]);
    }
    if (p->pos + 1 < p->argc && is_binary(p->argv[p->pos])) {
        const char *op = p->argv[p->pos++];
        const char *right = p->argv[p->pos++];
        return binary(p, token, op, right);
    }
    return *token != '\0';
}

static bool parse_not(parser_t *p) {
    if (p->pos < p->argc && strcmp(p->argv[p->pos], "!") == 0) {
        p->pos++;
        return !parse_not(p);
    }
    return parse_primary(p);
}

static bool parse_and(parser_t *p) {
    bool result = parse_not(p);
    while (p->pos < p->argc && strcmp(p->argv[p->pos], "-a") == 0) {
        p->pos++;
        bool right = parse_not(p);
        result = result && right;
    }
    return result;
}

static bool parse_or(parser_t *p) {
    bool result = parse_and(p);
    while (p->pos < p->argc && strcmp(p->argv[p->pos], "-o") == 0) {
        p->pos++;
        bool right = parse_and(p);
        result = result || right;
    }
    return result;
}

int main(int argc, char **argv) {
    kx_prog = "test";
    int first = 1;
    if (first < argc && strcmp(argv[first], "--") == 0) first++;
    if (first == argc) return 1;
    parser_t p = { .argc = argc, .argv = argv, .pos = first, .error = false };
    bool result = parse_or(&p);
    if (p.error || p.pos != argc) {
        fprintf(stderr, "test: syntax error\n");
        return 2;
    }
    return result ? 0 : 1;
}
