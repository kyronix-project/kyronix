#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "cp";
    if (argc != 3)
        kx_die("usage: cp SRC DST");
    if (kx_copy_file(argv[1], argv[2]) < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}
