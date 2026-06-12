#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "touch";
    if (argc < 2)
        kx_die("missing operand");
    int rc = 0;
    for (int i = 1; i < argc; i++)
    {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
        if (fd < 0)
        {
            kx_warn(argv[i]);
            rc = 1;
        }
        else
            close(fd);
    }
    return rc;
}
