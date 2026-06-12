#include "common.h"
extern char** environ;
int main(int argc, char** argv)
{
    if (argc == 1)
    {
        for (char** e = environ; *e; e++)
            puts(*e);
        return 0;
    }
    int rc = 1;
    for (int i = 1; i < argc; i++)
    {
        char* v = getenv(argv[i]);
        if (v)
        {
            puts(v);
            rc = 0;
        }
    }
    return rc;
}
