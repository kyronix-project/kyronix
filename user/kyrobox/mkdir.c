#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "mkdir";
    bool parents = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-p") == 0) {
        parents = true;
        first = 2;
    }
    if (first == argc)
        kx_die("missing operand");
    int rc = 0;
    for (int i = first; i < argc; i++) {
        int r = parents ? kx_mkdir_p(argv[i], 0777) : mkdir(argv[i], 0777);
        if (r < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
