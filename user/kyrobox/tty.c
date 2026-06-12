#include "common.h"
int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    char* n = ttyname(STDIN_FILENO);
    puts(n ? n : "not a tty");
    return n ? 0 : 1;
}
