#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "head";
    long max = 10;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        max = strtol(argv[2], NULL, 10);
        first = 3;
    }
    if (first == argc)
        argv[argc++] = NULL;
    int rc = 0;
    for (int a = first; a < argc; a++) {
        FILE* f = argv[a] ? fopen(argv[a], "r") : stdin;
        if (!f) {
            kx_warn(argv[a]);
            rc = 1;
            continue;
        }
        char* line = NULL;
        size_t cap = 0;
        long n = 0;
        while (n++ < max && getline(&line, &cap, f) >= 0)
            fputs(line, stdout);
        free(line);
        if (argv[a])
            fclose(f);
    }
    return rc;
}
