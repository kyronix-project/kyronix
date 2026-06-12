#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "mktemp";
    const char* tmpl = argc > 1 ? argv[1] : "tmp.XXXXXX";
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", tmpl);
    char* x = strstr(path, "XXXXXX");
    if (!x)
        kx_die("template must contain XXXXXX");
    for (int i = 0; i < 1000; i++)
    {
        unsigned v = (unsigned) (((long) getpid() + time(NULL) + i) % 1000000L);
        for (int j = 5; j >= 0; j--)
        {
            x[j] = (char) ('0' + (v % 10));
            v /= 10;
        }
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd >= 0)
        {
            close(fd);
            puts(path);
            return 0;
        }
    }
    kx_warn(path);
    return 1;
}
