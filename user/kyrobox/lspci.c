#include "common.h"

static int copy_file_to_stdout(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        kx_warn(path);
        return 1;
    }
    char buf[1024];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            kx_warn(path);
            close(fd);
            return 1;
        }
        if (n == 0) break;
        char *p = buf;
        ssize_t left = n;
        while (left > 0) {
            ssize_t written = write(STDOUT_FILENO, p, (size_t)left);
            if (written < 0) {
                if (errno == EINTR) continue;
                kx_warn("stdout");
                close(fd);
                return 1;
            }
            p += written;
            left -= written;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    kx_prog = "lspci";
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: lspci\n");
        return 2;
    }
    return copy_file_to_stdout("/proc/bus/pci/devices");
}
