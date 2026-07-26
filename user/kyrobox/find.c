#include "common.h"
#include <fnmatch.h>

static const char *name_pat = NULL;
static const char *type_filter = NULL;
static const char *exec_cmd = NULL;

static int walk(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        return 1;
    }

    int match = 1;
    if (name_pat && fnmatch(name_pat, kx_base(path), 0) != 0) match = 0;
    if (type_filter) {
        char ft = 'f';
        if (S_ISDIR(st.st_mode)) ft = 'd';
        else if (S_ISLNK(st.st_mode)) ft = 'l';
        else if (S_ISBLK(st.st_mode)) ft = 'b';
        else if (S_ISCHR(st.st_mode)) ft = 'c';
        else if (S_ISFIFO(st.st_mode)) ft = 'p';
        else if (S_ISSOCK(st.st_mode)) ft = 's';
        if (ft != type_filter[0]) match = 0;
    }

    if (match) {
        if (exec_cmd) {
            char cmd[PATH_MAX + 1024];
            const char *p = exec_cmd;
            char *dst = cmd;
            char *end = cmd + sizeof(cmd) - 1;
            while (*p && dst < end) {
                if (*p == '{' && p[1] == '}') {
                    size_t plen = strlen(path);
                    if (dst + plen < end) {
                        memcpy(dst, path, plen);
                        dst += plen;
                    }
                    p += 2;
                } else {
                    *dst++ = *p++;
                }
            }
            *dst = '\0';
            int rc = system(cmd);
            if (rc != 0) return rc;
        } else {
            puts(path);
        }
    }

    if (!S_ISDIR(st.st_mode)) return 0;
    DIR *d = opendir(path);
    if (!d) {
        kx_warn(path);
        return 1;
    }
    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        rc |= walk(child);
    }
    closedir(d);
    return rc;
}

int main(int argc, char **argv) {
    kx_prog = "find";
    const char *root = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-') root = argv[i++];
    while (i < argc) {
        if (strcmp(argv[i], "-name") == 0 && i + 1 < argc) {
            name_pat = argv[++i];
        } else if (strcmp(argv[i], "-type") == 0 && i + 1 < argc) {
            type_filter = argv[++i];
        } else if (strcmp(argv[i], "-exec") == 0 && i + 1 < argc) {
            exec_cmd = argv[++i];
        } else {
            kx_die("usage: find [PATH] [-name PATTERN] [-type f|d|l] [-exec CMD {}]");
        }
        i++;
    }
    return walk(root);
}
