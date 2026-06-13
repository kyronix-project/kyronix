#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "basename";
    if (argc < 2)
        kx_die("missing operand");
    puts(kx_base(argv[1]));
    return 0;
}
