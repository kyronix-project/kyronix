#include "common.h"
static int cmp_lines(const void* a, const void* b)
{
    const char *const *sa = a, *const *sb = b;
    return strcmp(*sa, *sb);
}
int main(int argc, char** argv)
{
    kx_prog = "sort";
    if (argc == 1)
        argv[argc++] = NULL;
    char** lines = NULL;
    size_t nlines = 0, caplines = 0;
    int rc = 0;
    for (int a = 1; a < argc; a++)
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
            if (nlines == caplines)
            {
                caplines = caplines ? caplines * 2 : 64;
                lines = realloc(lines, caplines * sizeof(*lines));
            }
            lines[nlines++] = strdup(line);
        }
        free(line);
        if (argv[a])
            fclose(f);
    }
    qsort(lines, nlines, sizeof(*lines), cmp_lines);
    for (size_t i = 0; i < nlines; i++)
    {
        fputs(lines[i], stdout);
        free(lines[i]);
    }
    free(lines);
    return rc;
}
