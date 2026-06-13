#include "common.h"
#include <grp.h>
int main(int argc, char** argv)
{
    kx_prog = "chgrp";
    if (argc < 3)
        kx_die("usage: chgrp GROUP FILE...");
    char* end = NULL;
    gid_t gid = (gid_t) strtoul(argv[1], &end, 10);
    if (!end || *end) {
        struct group* gr = getgrnam(argv[1]);
        if (!gr)
            kx_die("bad group");
        gid = gr->gr_gid;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], (uid_t) -1, gid) < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
