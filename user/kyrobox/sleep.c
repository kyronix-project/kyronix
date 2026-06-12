#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "sleep";
    if (argc < 2)
        kx_die("missing operand");
    for (int i = 1; i < argc; i++)
        sleep((unsigned) strtoul(argv[i], NULL, 10));
    return 0;
}
