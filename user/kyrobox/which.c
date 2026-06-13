#include "common.h"
int main(int argc, char** argv)
{
    kx_prog = "which";
    if (argc < 2)
        kx_die("missing operand");
    const char* path = getenv("PATH");
    if (!path)
        path = "/bin:/usr/bin";
    int rc = 1;
    for (int a = 1; a < argc; a++) {
        char* dup = strdup(path);
        for (char *save = NULL, *dir = strtok_r(dup, ":", &save); dir;
             dir = strtok_r(NULL, ":", &save)) {
            char full[PATH_MAX];
            snprintf(full, sizeof(full), "%s/%s", dir, argv[a]);
            if (access(full, X_OK) == 0) {
                puts(full);
                rc = 0;
                break;
            }
        }
        free(dup);
    }
    return rc;
}
