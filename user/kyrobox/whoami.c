#include "common.h"
int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    struct passwd* pw = getpwuid(geteuid());
    puts(pw ? pw->pw_name : "unknown");
    return 0;
}
