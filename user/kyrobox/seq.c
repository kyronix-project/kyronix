#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "seq";
    if (argc < 2 || argc > 4)
        kx_die("usage: seq [FIRST [STEP]] LAST");
    long first = 1, step = 1, last;
    if (argc == 2)
        last = strtol(argv[1], NULL, 10);
    else if (argc == 3)
    {
        first = strtol(argv[1], NULL, 10);
        last = strtol(argv[2], NULL, 10);
    }
    else
    {
        first = strtol(argv[1], NULL, 10);
        step = strtol(argv[2], NULL, 10);
        last = strtol(argv[3], NULL, 10);
    }
    if (step == 0)
        kx_die("zero step");
    if (step > 0)
        for (long i = first; i <= last; i += step)
            printf("%ld\n", i);
    else
        for (long i = first; i >= last; i += step)
            printf("%ld\n", i);
    return 0;
}
