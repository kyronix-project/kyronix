#include "common.h"
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    kx_prog = "grep";
    bool only_matching = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-o") == 0) {
        only_matching = true;
        first = 2;
    }
    if (argc < first + 1) kx_die("usage: grep [-o] PATTERN [FILE...]");
    const char *pat = argv[first];
    first++;
    if (first == argc) argv[argc++] = NULL;
    int matched = 0, rc = 1;
    for (int a = first; a < argc; a++) {
        FILE *f = argv[a] ? fopen(argv[a], "r") : stdin;
        if (!f) {
            kx_warn(argv[a]);
            rc = 2;
            continue;
        }
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, f) >= 0) {
            char *hit = strstr(line, pat);
            if (hit) {
                if (only_matching) {
                    fputs(pat, stdout);
                    fputc('\n', stdout);
                } else {
                    fputs(line, stdout);
                }
                matched = 1;
            }
        }
        free(line);
        if (argv[a]) fclose(f);
    }
    return matched ? 0 : rc;
}
