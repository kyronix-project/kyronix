#include "common.h"
int main(int argc, char** argv)
{
    (void) argc;
    (void) argv;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[128];
    strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Y", &tm);
    puts(buf);
    return 0;
}
