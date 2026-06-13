#include "common.h"
int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    char cwd[PATH_MAX];
    kx_prog = "pwd";
    if (!getcwd(cwd, sizeof(cwd))) {
        kx_warn(".");
        return 1;
    }
    puts(cwd);
    return 0;
}
