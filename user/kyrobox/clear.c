#include "common.h"
int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    fputs("\033[H\033[2J", stdout);
    return 0;
}
