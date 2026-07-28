#include "common.h"

int main(int argc, char **argv) {
    kx_prog = "tty";
    bool silent = false;
    int i = 1;
    for (; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        }
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--silent") == 0 ||
            strcmp(argv[i], "--quiet") == 0) {
            silent = true;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0) {
            puts("usage: tty [-s]");
            return 0;
        }
        fprintf(stderr, "tty: invalid option: %s\n", argv[i]);
        return 2;
    }
    if (i != argc) return 2;
    char *name = ttyname(STDIN_FILENO);
    if (!silent) puts(name ? name : "not a tty");
    return name ? 0 : 1;
}
