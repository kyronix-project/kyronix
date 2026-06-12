#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "dirname";
    if (argc < 2)
        kx_die("missing operand");
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", argv[1]);
    char* p = strrchr(tmp, '/');
    if (!p)
        puts(".");
    else if (p == tmp)
        puts("/");
    else
    {
        *p = 0;
        puts(tmp);
    }
    return 0;
}
