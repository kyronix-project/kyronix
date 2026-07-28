#define _GNU_SOURCE
#include "common.h"

static void usage(void) {
    fprintf(stderr, "usage: sync [-d|-f] [FILE...]\n");
    exit(1);
}

int main(int argc, char **argv) {
    kx_prog = "sync";
    int mode = 0;
    int first = 1;

    if (first < argc && strcmp(argv[first], "--help") == 0) {
        puts("usage: sync [-d|-f] [FILE...]");
        return 0;
    }
    if (first < argc && strcmp(argv[first], "-d") == 0) {
        mode = 1;
        first++;
    } else if (first < argc && strcmp(argv[first], "-f") == 0) {
        mode = 2;
        first++;
    } else if (first < argc && argv[first][0] == '-') {
        usage();
    }

    if (first == argc) {
        if (mode != 0) usage();
        sync();
        return 0;
    }

    int rc = 0;
    for (int i = first; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            kx_warn(argv[i]);
            rc = 1;
            continue;
        }
        int result = mode == 1 ? fdatasync(fd) : mode == 2 ? syncfs(fd) : fsync(fd);
        if (result < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
        close(fd);
    }
    return rc;
}
