#include "common.h"

#ifndef CLONE_NEWNS
#define CLONE_NEWNS 0x00020000
#endif
#ifndef CLONE_NEWUTS
#define CLONE_NEWUTS 0x04000000
#endif
#ifndef CLONE_NEWIPC
#define CLONE_NEWIPC 0x08000000
#endif

extern int unshare(int flags);

static void usage(void) {
    fprintf(stderr, "usage: %s [-m] [-u] [-i] [--] cmd args...\n", kx_prog);
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);
    int flags = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && strcmp(argv[i], "--") != 0; i++) {
        if (strcmp(argv[i], "-m") == 0) flags |= CLONE_NEWNS;
        else if (strcmp(argv[i], "-u") == 0) flags |= CLONE_NEWUTS;
        else if (strcmp(argv[i], "-i") == 0) flags |= CLONE_NEWIPC;
        else { usage(); return 1; }
    }
    if (strcmp(argv[i], "--") == 0) i++;
    if (i >= argc) { usage(); return 1; }
    if (unshare(flags) < 0) {
        kx_warn("unshare");
        return 1;
    }
    execvp(argv[i], argv + i);
    kx_warn(argv[i]);
    return 127;
}
