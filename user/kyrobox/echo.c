#include "common.h"
int main(int argc, char** argv)
{
    bool nl = true;
    int i = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0)
    {
        nl = false;
        i = 2;
    }
    for (; i < argc; i++)
    {
        if (i > (nl ? 1 : 2))
            putchar(' ');
        fputs(argv[i], stdout);
    }
    if (nl)
        putchar('\n');
    return 0;
}
