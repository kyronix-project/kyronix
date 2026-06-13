#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "link";
    if (argc != 3)
        kx_die("usage: link FILE LINK");
    if (link(argv[1], argv[2]) < 0) {
        kx_warn(argv[2]);
        return 1;
    }
    return 0;
}
