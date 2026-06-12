#include "common.h"
int main(int argc, char** argv)
{
    const char* s = argc > 1 ? argv[1] : "y";
    for (;;)
        puts(s);
    return 0;
}
