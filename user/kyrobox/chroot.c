#define _DEFAULT_SOURCE
#include "common.h"

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);
    if (argc < 2) {
        fprintf(stderr, "usage: %s <newroot> [cmd args...]\n", kx_prog);
        return 1;
    }
    const char *root = argv[1];
    if (chroot(root) < 0) {
        kx_warn(root);
        return 1;
    }
    if (chdir("/") < 0) {
        kx_warn("/");
        return 1;
    }
    if (argc < 3) return 0;
    execvp(argv[2], argv + 2);
    kx_warn(argv[2]);
    return 127;
}
