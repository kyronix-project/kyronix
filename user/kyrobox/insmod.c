#define _GNU_SOURCE
#include "common.h"
#include <sys/syscall.h>

int main(int argc, char **argv) {
    kx_prog = "insmod";
    if (argc < 2) {
        fprintf(stderr, "usage: insmod MODULE [PARAMETER...]\n");
        return 1;
    }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        kx_warn(argv[1]);
        return 1;
    }

    char params[4096];
    size_t used = 0;
    params[0] = '\0';
    for (int i = 2; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (used + (used ? 1 : 0) + n >= sizeof(params)) {
            fprintf(stderr, "insmod: parameter list too long\n");
            close(fd);
            return 1;
        }
        if (used) params[used++] = ' ';
        memcpy(params + used, argv[i], n);
        used += n;
        params[used] = '\0';
    }

    long rc = syscall(SYS_finit_module, fd, params, 0);
    int saved_errno = errno;
    close(fd);
    if (rc < 0) {
        errno = saved_errno;
        kx_warn(argv[1]);
        return 1;
    }
    return 0;
}
