#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "tr";
    bool del = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-d") == 0) {
        del = true;
        first = 2;
    }
    if ((!del && argc - first < 2) || (del && argc - first < 1))
        kx_die("usage: tr [-d] SET1 [SET2]");
    const char *s1 = argv[first], *s2 = del ? "" : argv[first + 1];
    int c;
    while ((c = getchar()) != EOF) {
        char* p = strchr(s1, c);
        if (p) {
            if (del)
                continue;
            size_t idx = (size_t) (p - s1);
            putchar(s2[idx < strlen(s2) ? idx : strlen(s2) - 1]);
        } else
            putchar(c);
    }
    return 0;
}
