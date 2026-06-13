#include "common.h"
int main(int argc, char** argv)
{
    if (argc == 1)
        return 1;
    if (argc == 2)
        return argv[1][0] ? 0 : 1;
    if (argc == 3) {
        struct stat st;
        if (strcmp(argv[1], "-f") == 0)
            return stat(argv[2], &st) == 0 && S_ISREG(st.st_mode) ? 0 : 1;
        if (strcmp(argv[1], "-d") == 0)
            return stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode) ? 0 : 1;
        if (strcmp(argv[1], "-e") == 0)
            return stat(argv[2], &st) == 0 ? 0 : 1;
        if (strcmp(argv[1], "-n") == 0)
            return argv[2][0] ? 0 : 1;
        if (strcmp(argv[1], "-z") == 0)
            return argv[2][0] ? 1 : 0;
    }
    if (argc == 4) {
        if (strcmp(argv[2], "=") == 0)
            return strcmp(argv[1], argv[3]) == 0 ? 0 : 1;
        if (strcmp(argv[2], "!=") == 0)
            return strcmp(argv[1], argv[3]) != 0 ? 0 : 1;
        if (strcmp(argv[2], "-eq") == 0)
            return strtol(argv[1], NULL, 10) == strtol(argv[3], NULL, 10) ? 0 : 1;
    }
    return 1;
}
