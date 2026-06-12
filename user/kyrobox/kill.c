#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "kill";
    int sig = SIGTERM;
    int first = 1;
    if (argc > 2 && argv[1][0] == '-')
    {
        sig = atoi(argv[1] + 1);
        first = 2;
    }
    if (first == argc)
        kx_die("missing pid");
    int rc = 0;
    for (int i = first; i < argc; i++)
    {
        if (kill((pid_t) atoi(argv[i]), sig) < 0)
        {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
