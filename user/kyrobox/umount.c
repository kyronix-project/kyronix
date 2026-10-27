#include "common.h"
#include <sys/mount.h>

static void usage(void) { fprintf(stderr, "usage: %s [-f] [-l] mountpoint\n", kx_prog); }

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);
    int flags = 0;
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-') {
        if (strcmp(argv[ai], "-f") == 0)
            flags |= MNT_FORCE;
        else if (strcmp(argv[ai], "-l") == 0)
            flags |= MNT_DETACH;
        else if (strcmp(argv[ai], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
        ai++;
    }

    if (ai >= argc) {
        usage();
        return 1;
    }

    int rc = 0;
    for (int i = ai; i < argc; i++) {
        if (umount2(argv[i], flags) < 0) {
            fprintf(stderr, "%s: %s: %s\n", kx_prog, argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}
