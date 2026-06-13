#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "tail";
    long max = 10;
    int first = 1;
    if (argc > 2 && strcmp(argv[1], "-n") == 0) {
        max = strtol(argv[2], NULL, 10);
        first = 3;
    }
    if (max < 0)
        max = 0;
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
        char** ring = calloc((size_t) max + 1, sizeof(char*));
        char* line = NULL;
        size_t cap = 0;
        long n = 0;
        while (getline(&line, &cap, f) >= 0 && max > 0) {
            free(ring[n % max]);
            ring[n % max] = strdup(line);
            n++;
        }
        long start = n > max ? n - max : 0;
        for (long i = start; i < n; i++)
            fputs(ring[i % max], stdout);
        for (long i = 0; i < max; i++)
            free(ring[i]);
        free(ring);
        free(line);
        if (argv[a])
            fclose(f);
    }
    return rc;
}
