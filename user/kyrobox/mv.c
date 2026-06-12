#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "mv";
    if (argc != 3)
        kx_die("usage: mv SRC DST");
    if (rename(argv[1], argv[2]) == 0)
        return 0;
    if (kx_copy_file(argv[1], argv[2]) == 0 && unlink(argv[1]) == 0)
        return 0;
    kx_warn(argv[1]);
    return 1;
}
