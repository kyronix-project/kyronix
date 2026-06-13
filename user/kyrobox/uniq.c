#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "uniq";
    FILE* f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) {
        kx_warn(argv[1]);
        return 1;
    }
    char *line = NULL, *prev = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) >= 0) {
        if (!prev || strcmp(prev, line) != 0)
            fputs(line, stdout);
        free(prev);
        prev = strdup(line);
    }
    free(prev);
    free(line);
    if (argc > 1)
        fclose(f);
    return 0;
}
