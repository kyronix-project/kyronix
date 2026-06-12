#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "cut";
    if (argc < 3 || strcmp(argv[1], "-c") != 0)
        kx_die("usage: cut -c N[-M] [FILE...]");
    long from = strtol(argv[2], NULL, 10), to = from;
    char* dash = strchr(argv[2], '-');
    if (dash && dash[1])
        to = strtol(dash + 1, NULL, 10);
    if (from < 1)
        from = 1;
    if (to < from)
        to = LONG_MAX;
    int first = 3;
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
        char* line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) >= 0)
        {
            long col = 1;
            for (char* p = line; *p && *p != '\n'; p++, col++)
                if (col >= from && col <= to)
                    putchar(*p);
            putchar('\n');
        }
        free(line);
        if (argv[a])
            fclose(f);
    }
    return rc;
}
