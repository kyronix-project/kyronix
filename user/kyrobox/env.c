#include "common.h"
extern char** environ;
int main(int argc, char** argv)
{
    kx_prog = "env";
    if (argc == 1) {
        for (char** e = environ; *e; e++)
            puts(*e);
        return 0;
    }
    execvp(argv[1], argv + 1);
    kx_warn(argv[1]);
    return 127;
}
