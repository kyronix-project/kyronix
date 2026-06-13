#include "common.h"
#include <fnmatch.h>

static const char* name_pat;

static int walk(const char* path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        return 1;
    }
    if (!name_pat || fnmatch(name_pat, kx_base(path), 0) == 0)
        puts(path);
    if (!S_ISDIR(st.st_mode))
        return 0;
    DIR* d = opendir(path);
    if (!d) {
        kx_warn(path);
        return 1;
    }
    int rc = 0;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        rc |= walk(child);
    }
    closedir(d);
    return rc;
}

int main(int argc, char** argv)
{
    kx_prog = "find";
    const char* root = ".";
    int i = 1;
    if (i < argc && argv[i][0] != '-')
        root = argv[i++];
    if (i < argc) {
        if (i + 1 < argc && strcmp(argv[i], "-name") == 0)
            name_pat = argv[i + 1];
        else
            kx_die("usage: find [PATH] [-name PATTERN]");
    }
    return walk(root);
}
