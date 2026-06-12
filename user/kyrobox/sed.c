#include "common.h"

static int run_stream(FILE* f, const char* old, const char* new, bool global)
{
    char* line = NULL;
    size_t cap = 0;
    size_t oldlen = strlen(old);
    while (getline(&line, &cap, f) >= 0)
    {
        char* p = line;
        while (*p)
        {
            char* m = strstr(p, old);
            if (!m || oldlen == 0)
            {
                fputs(p, stdout);
                break;
            }
            fwrite(p, 1, (size_t) (m - p), stdout);
            fputs(new, stdout);
            p = m + oldlen;
            if (!global)
            {
                fputs(p, stdout);
                break;
            }
        }
    }
    free(line);
    return 0;
}

int main(int argc, char** argv)
{
    kx_prog = "sed";
    if (argc < 2)
        kx_die("usage: sed 's/OLD/NEW/[g]' [FILE...]");
    const char* expr = argv[1];
    if (strncmp(expr, "s/", 2) != 0)
        kx_die("only s/OLD/NEW/[g] supported");
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", expr + 2);
    char* old = buf;
    char* slash = strchr(old, '/');
    if (!slash)
        kx_die("bad expression");
    *slash++ = 0;
    char* new = slash;
    slash = strchr(new, '/');
    if (!slash)
        kx_die("bad expression");
    *slash++ = 0;
    bool global = strchr(slash, 'g') != NULL;
    int first = 2;
    if (first == argc)
        argv[argc++] = NULL;
    int rc = 0;
    for (int i = first; i < argc; i++)
    {
        FILE* f = argv[i] ? fopen(argv[i], "r") : stdin;
        if (!f)
        {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        run_stream(f, old, new, global);
        if (argv[i])
            fclose(f);
    }
    return rc;
}
