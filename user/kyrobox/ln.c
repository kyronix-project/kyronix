#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "ln";
    bool sym = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        sym = true;
        first = 2;
    }
    if (argc - first != 2)
        kx_die("usage: ln [-s] TARGET LINK");
    if ((sym ? symlink(argv[first], argv[first + 1]) : link(argv[first], argv[first + 1])) < 0) {
        kx_warn(argv[first + 1]);
        return 1;
    }
    return 0;
}
