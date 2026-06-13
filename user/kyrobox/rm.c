#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "rm";
    bool force = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        force = true;
        first = 2;
    }
    if (first == argc && !force)
        kx_die("missing operand");
    int rc = 0;
    for (int i = first; i < argc; i++) {
        if (unlink(argv[i]) < 0) {
            if (force && errno == ENOENT)
                continue;
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
