#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "readlink";
    if (argc != 2)
        kx_die("usage: readlink LINK");
    char buf[PATH_MAX];
    ssize_t n = readlink(argv[1], buf, sizeof(buf) - 1);
    if (n < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    buf[n] = 0;
    puts(buf);
    return 0;
}
