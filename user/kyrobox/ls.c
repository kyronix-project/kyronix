#include "common.h"
#include <sys/ioctl.h>
#include <unistd.h>
static char ftype(mode_t m)
{
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISCHR(m))
        return 'c';
    return '-';
}
static void mode_string(mode_t m, char out[11])
{
    out[0] = ftype(m);
    const char c[] = "rwx";
    for (int i = 0; i < 9; i++)
        out[i + 1] = (m & (1 << (8 - i))) ? c[i % 3] : '-';
    out[10] = 0;
}
static int list_one(const char* path, bool longfmt, bool head)
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
            printf("%s %5ld %s\n", m, (long) st.st_size, kx_base(path));
        } else
            puts(kx_base(path));
        return 0;
    }
    int one_per_line = !isatty(STDOUT_FILENO);
    DIR* d = opendir(path);
    if (!d) {
        kx_warn(path);
        return 1;
    }
    if (head)
        printf("%s:\n", path);
    struct dirent* de;
    if (longfmt) {
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
            if (lstat(full, &st) == 0) {
                char m[11];
                mode_string(st.st_mode, m);
                printf("%s %5ld %s\n", m, (long) st.st_size, de->d_name);
            }
        }
        closedir(d);
        return 0;
    }
    if (one_per_line) {
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.')
                continue;
            puts(de->d_name);
        }
        closedir(d);
        return 0;
    }
    char** names = NULL;
    int count = 0, cap = 0;
    int namemax = 0;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        int len = strlen(de->d_name);
        if (len > namemax)
            namemax = len;
        if (count >= cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof(char*));
        }
        names[count++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, count, sizeof(char*), (int (*)(const void*, const void*)) strcmp);
    int cols = 80;
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        cols = ws.ws_col;
    int colw = namemax + 2;
    int ncols = cols / colw;
    if (ncols < 1)
        ncols = 1;
    int rows = (count + ncols - 1) / ncols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < ncols; c++) {
            int i = c * rows + r;
            if (i >= count)
                break;
            if (c == ncols - 1)
                fputs(names[i], stdout);
            else
                printf("%-*s", colw, names[i]);
        }
        putchar('\n');
    }
    for (int i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return 0;
}
int main(int argc, char** argv)
{
    kx_prog = "ls";
    bool longfmt = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        longfmt = true;
        first = 2;
    }
    if (first == argc)
        return list_one(".", longfmt, false);
    int rc = 0;
    bool multi = argc - first > 1;
    for (int i = first; i < argc; i++)
        rc |= list_one(argv[i], longfmt, multi);
    return rc;
}
