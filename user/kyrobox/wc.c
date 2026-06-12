#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "wc";
    int first = 1;
    if (argc > 1 && argv[1][0] == '-')
        first = 2;
    if (first == argc)
        argv[argc++] = NULL;
    int rc = 0;
    for (int a = first; a < argc; a++)
    {
        FILE* f = argv[a] ? fopen(argv[a], "r") : stdin;
        if (!f)
        {
            kx_warn(argv[a]);
            rc = 1;
            continue;
        }
        long lines = 0, words = 0, bytes = 0;
        int inword = 0, c;
        while ((c = fgetc(f)) != EOF)
        {
            bytes++;
            if (c == '\n')
                lines++;
            if (isspace(c))
                inword = 0;
            else if (!inword)
            {
                words++;
                inword = 1;
            }
        }
        printf("%ld %ld %ld", lines, words, bytes);
        if (argv[a])
            printf(" %s", argv[a]);
        putchar('\n');
        if (argv[a])
            fclose(f);
    }
    return rc;
}
