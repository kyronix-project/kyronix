#include "common.h"

extern char *realpath(const char *restrict path, char *restrict resolved);

static int lexical_path(const char *path, char *output, size_t size) {
    char absolute[PATH_MAX];
    if (path[0] == '/') {
        if (snprintf(absolute, sizeof(absolute), "%s", path) >= (int) sizeof(absolute)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return -1;
        if (snprintf(absolute, sizeof(absolute), "%s/%s", cwd, path) >= (int) sizeof(absolute)) {
            errno = ENAMETOOLONG;
            return -1;
        }
    }

    char *parts[PATH_MAX / 2];
    size_t count = 0;
    char *save = NULL;
    for (char *part = strtok_r(absolute, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || part[0] == '\0') continue;
        if (strcmp(part, "..") == 0) {
            if (count) count--;
        } else {
            parts[count++] = part;
        }
    }
    size_t used = 0;
    if (size < 2) {
        errno = ENAMETOOLONG;
        return -1;
    }
    output[used++] = '/';
    for (size_t i = 0; i < count; i++) {
        size_t len = strlen(parts[i]);
        if (used + len + (i + 1 < count ? 1 : 0) >= size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(output + used, parts[i], len);
        used += len;
        if (i + 1 < count) output[used++] = '/';
    }
    output[used] = '\0';
    return 0;
}

static int canonical_missing_final(const char *path, char *output) {
    char copy[PATH_MAX];
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int) sizeof(copy)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(copy, '/');
    const char *base = slash ? slash + 1 : copy;
    char parent[PATH_MAX];
    if (slash) {
        if (slash == copy)
            strcpy(parent, "/");
        else {
            *slash = '\0';
            strcpy(parent, copy);
        }
    } else {
        strcpy(parent, ".");
    }
    char resolved[PATH_MAX];
    if (!realpath(parent, resolved)) return -1;
    int n = snprintf(output, PATH_MAX, "%s%s%s", resolved,
                     resolved[strlen(resolved) - 1] == '/' ? "" : "/", base);
    if (n < 0 || n >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "readlink";
    enum { RAW, CANON_FINAL, CANON_ALL, CANON_MISSING } mode = RAW;
    bool newline = true;
    bool quiet = true;
    bool zero = false;
    int first = 1;

    for (; first < argc; first++) {
        const char *arg = argv[first];
        if (strcmp(arg, "--") == 0) {
            first++;
            break;
        }
        if (arg[0] != '-' || arg[1] == '\0') break;
        if (strcmp(arg, "--help") == 0) {
            puts("usage: readlink [-fnemzv] FILE...");
            return 0;
        }
        for (const char *p = arg + 1; *p; p++) {
            switch (*p) {
            case 'f': mode = CANON_FINAL; break;
            case 'e': mode = CANON_ALL; break;
            case 'm': mode = CANON_MISSING; break;
            case 'n': newline = false; break;
            case 'q':
            case 's': quiet = true; break;
            case 'v': quiet = false; break;
            case 'z': zero = true; newline = false; break;
            default: kx_die("invalid option");
            }
        }
    }
    if (first == argc) kx_die("missing operand");
    if (mode == RAW && argc - first != 1) kx_die("extra operand");

    int rc = 0;
    for (int i = first; i < argc; i++) {
        char result[PATH_MAX];
        bool ok = false;
        if (mode == RAW) {
            ssize_t n = readlink(argv[i], result, sizeof(result) - 1);
            if (n >= 0) {
                result[n] = '\0';
                ok = true;
            }
        } else if (mode == CANON_MISSING) {
            ok = lexical_path(argv[i], result, sizeof(result)) == 0;
        } else if (realpath(argv[i], result)) {
            ok = true;
        } else if (mode == CANON_FINAL && errno == ENOENT) {
            ok = canonical_missing_final(argv[i], result) == 0;
        }
        if (!ok) {
            if (!quiet) kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        fputs(result, stdout);
        if (zero)
            putchar('\0');
        else if (newline)
            putchar('\n');
    }
    return rc;
}
