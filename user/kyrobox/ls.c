#include "common.h"
#include <unistd.h>
static char ftype(mode_t m)
{
    if (S_ISDIR(m)) return 'd';
    if (S_ISLNK(m)) return 'l';
    if (S_ISCHR(m)) return 'c';
    return '-';
}
static void mode_string(mode_t m, char out[11])
{
    out[0] = ftype(m);
    const char c[] = "rwx";
    for (int i = 0; i < 9; i++) out[i + 1] = (m & (1 << (8 - i))) ? c[i % 3] : '-';
    out[10] = 0;
}
static int list_one(const char *path, bool longfmt, bool head)
{
    struct stat st;
    if (lstat(path, &st) < 0) {
        kx_warn(path);
        return 1;
    }
    if (!S_ISDIR(st.st_mode)) {
        if (longfmt) {
            char m[11];
            mode_string(st.st_mode, m);
            printf("%s %5ld %s\n", m, (long)st.st_size, kx_base(path));
        } else puts(kx_base(path));
        return 0;
    }
    int one_per_line = !isatty(STDOUT_FILENO);
    DIR *d = opendir(path);
    if (!d) {
        kx_warn(path);
        return 1;
    }
    if (head) printf("%s:\n", path);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        if (!longfmt) {
            if (one_per_line)
                puts(de->d_name);
            else
                printf("%s  ", de->d_name);
            continue;
        }
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        if (lstat(full, &st) == 0) {
            char m[11];
            mode_string(st.st_mode, m);
            printf("%s %5ld %s\n", m, (long)st.st_size, de->d_name);
        }
    }
    if (!longfmt && !one_per_line) putchar('\n');
    closedir(d);
    return 0;
}
int main(int argc, char **argv)
{
    kx_prog = "ls";
    bool longfmt = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        longfmt = true;
        first = 2;
    }
    if (first == argc) return list_one(".", longfmt, false);
    int rc = 0;
    bool multi = argc - first > 1;
    for (int i = first; i < argc; i++) rc |= list_one(argv[i], longfmt, multi);
    return rc;
}
