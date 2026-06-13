#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "unlink";
    if (argc != 2)
        kx_die("usage: unlink FILE");
    if (unlink(argv[1]) < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}
