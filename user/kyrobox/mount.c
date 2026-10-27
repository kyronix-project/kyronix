#include "common.h"
#include <sys/mount.h>

static void usage(void) {
    fprintf(stderr, "usage: %s [-t type] [-o options] device mountpoint\n", kx_prog);
    fprintf(stderr, "\noptions: ro,rw,nosuid,nodev,noexec,sync,remount,noatime,nodiratime\n");
}

static unsigned long parse_flags(const char *opt) {
    unsigned long flags = 0;
    char tmp[256];
    strncpy(tmp, opt, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = 0;
    char *save = NULL;
    char *tok = strtok_r(tmp, ",", &save);
    while (tok) {
        if (strcmp(tok, "ro") == 0)
            flags |= MS_RDONLY;
        else if (strcmp(tok, "rw") == 0)
            flags &= ~MS_RDONLY;
        else if (strcmp(tok, "nosuid") == 0)
            flags |= MS_NOSUID;
        else if (strcmp(tok, "nodev") == 0)
            flags |= MS_NODEV;
        else if (strcmp(tok, "noexec") == 0)
            flags |= MS_NOEXEC;
        else if (strcmp(tok, "sync") == 0)
            flags |= MS_SYNCHRONOUS;
        else if (strcmp(tok, "remount") == 0)
            flags |= MS_REMOUNT;
        else if (strcmp(tok, "noatime") == 0)
            flags |= MS_NOATIME;
        else if (strcmp(tok, "nodiratime") == 0)
            flags |= MS_NODIRATIME;
        else
            fprintf(stderr, "%s: unknown option: %s\n", kx_prog, tok);
        tok = strtok_r(NULL, ",", &save);
    }
    return flags;
}

int main(int argc, char **argv) {
    kx_prog = kx_base(argv[0]);
    const char *type = NULL;
    const char *opt = NULL;
    int ai = 1;

    while (ai < argc && argv[ai][0] == '-') {
        if (strcmp(argv[ai], "-t") == 0 && ai + 1 < argc) {
            type = argv[++ai];
        } else if (strcmp(argv[ai], "-o") == 0 && ai + 1 < argc) {
            opt = argv[++ai];
        } else if (strcmp(argv[ai], "--help") == 0) {
            usage();
            return 0;
        } else {
            usage();
            return 1;
        }
        ai++;
    }

    if (argc - ai < 2) {
        /* no arguments: show mounted filesystems */
        FILE *f = fopen("/proc/mounts", "r");
        if (!f) {
            kx_warn("/proc/mounts");
            return 1;
        }
        char line[256];
        while (fgets(line, sizeof(line), f)) { fputs(line, stdout); }
        fclose(f);
        return 0;
    }

    const char *dev = argv[ai];
    const char *mp = argv[ai + 1];

    unsigned long flags = opt ? parse_flags(opt) : 0;
    const char *data = NULL;

    if (mount(dev, mp, type, flags, data) < 0) {
        fprintf(stderr, "%s: %s on %s: %s\n", kx_prog, dev, mp, strerror(errno));
        return 1;
    }
    return 0;
}
