#include "common.h"

static long total_blocks(const char* path)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        return 0;
    }
    long total = (st.st_size + 1023) / 1024;
    if (!S_ISDIR(st.st_mode))
        return total;
    DIR* d = opendir(path);
    if (!d)
        return total;
    struct dirent* de;
    while ((de = readdir(d))) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        char child[PATH_MAX];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        total += total_blocks(child);
    }
    closedir(d);
    return total;
}

int main(int argc, char** argv)
{
    kx_prog = "du";
    if (argc == 1)
        argv[argc++] = ".";
    for (int i = 1; i < argc; i++)
        printf("%ld\t%s\n", total_blocks(argv[i]), argv[i]);
    return 0;
}
