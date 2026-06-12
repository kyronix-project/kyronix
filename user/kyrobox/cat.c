#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "cat";
    int rc = 0;
    if (argc == 1)
        return kx_copy_fd(STDIN_FILENO, STDOUT_FILENO) < 0;
    for (int i = 1; i < argc; i++)
    {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0)
        {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        if (kx_copy_fd(fd, STDOUT_FILENO) < 0)
        {
            kx_warn(argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
