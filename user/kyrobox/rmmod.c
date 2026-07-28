#define _GNU_SOURCE
#include "common.h"
#include <sys/syscall.h>

int main(int argc, char **argv) {
    kx_prog = "rmmod";
    if (argc != 2) {
        fprintf(stderr, "usage: rmmod MODULE\n");
        return 1;
    }
    if (syscall(SYS_delete_module, argv[1], 0) < 0) {
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}
