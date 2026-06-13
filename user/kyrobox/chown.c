#include "common.h"
#include <grp.h>

static int parse_id(const char* s, int group, uid_t* uid, gid_t* gid)
{
    char* end = NULL;
    unsigned long n = strtoul(s, &end, 10);
    if (end && *end == 0) {
        if (group)
            *gid = (gid_t) n;
        else
            *uid = (uid_t) n;
        return 0;
    }
    if (group) {
        struct group* gr = getgrnam(s);
        if (!gr)
            return -1;
        *gid = gr->gr_gid;
    } else {
        struct passwd* pw = getpwnam(s);
        if (!pw)
            return -1;
        *uid = pw->pw_uid;
    }
    return 0;
}

int main(int argc, char** argv)
{
    kx_prog = "chown";
    if (argc < 3)
        kx_die("usage: chown OWNER[:GROUP] FILE...");
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", argv[1]);
    char* colon = strchr(spec, ':');
    uid_t uid = (uid_t) -1;
    gid_t gid = (gid_t) -1;
    if (colon) {
        *colon++ = 0;
        if (*colon && parse_id(colon, 1, &uid, &gid) < 0)
            kx_die("bad group");
    }
    if (*spec && parse_id(spec, 0, &uid, &gid) < 0)
        kx_die("bad owner");
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            kx_warn(argv[i]);
            rc = 1;
        }
    }
    return rc;
}
