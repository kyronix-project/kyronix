#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "chmod";
    if (argc < 3)
        kx_die("usage: chmod MODE FILE...");
    char* end = NULL;
    long mode = strtol(argv[1], &end, 8);
    if (!end || *end)
        kx_die("bad mode");
    int rc = 0;
    for (int i = 2; i < argc; i++)
    {
        if (chmod(argv[i], (mode_t) mode) < 0)
        {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
