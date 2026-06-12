#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "dd";
    const char *ifile = NULL, *ofile = NULL;
    size_t bs = 512;
    long count = -1;
    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "if=", 3) == 0)
            ifile = argv[i] + 3;
        else if (strncmp(argv[i], "of=", 3) == 0)
            ofile = argv[i] + 3;
        else if (strncmp(argv[i], "bs=", 3) == 0)
            bs = strtoul(argv[i] + 3, NULL, 10);
        else if (strncmp(argv[i], "count=", 6) == 0)
            count = strtol(argv[i] + 6, NULL, 10);
    }
    FILE* in = ifile ? fopen(ifile, "rb") : stdin;
    FILE* out = ofile ? fopen(ofile, "wb") : stdout;
    if (!in)
    {
        kx_warn(ifile);
        return 1;
    }
    if (!out)
    {
        kx_warn(ofile);
        if (ifile)
            fclose(in);
        return 1;
    }
    char* buf = malloc(bs);
    if (!buf)
        return 1;
    long blocks = 0;
    size_t n;
    while ((count < 0 || blocks < count) && (n = fread(buf, 1, bs, in)) > 0)
    {
        if (fwrite(buf, 1, n, out) != n)
        {
            free(buf);
            return 1;
        }
        blocks++;
    }
    free(buf);
    if (ifile)
        fclose(in);
    if (ofile)
        fclose(out);
    return 0;
}
