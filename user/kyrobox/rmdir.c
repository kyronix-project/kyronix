#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "rmdir";
    if (argc < 2)
        kx_die("missing operand");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
