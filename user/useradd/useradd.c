#define _XOPEN_SOURCE 700
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void putstr(const char *s) { write(STDERR_FILENO, s, strlen(s)); }

static int next_uid(void) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 1000;
    char buf[256];
    int max = 999;
    while (fgets(buf, sizeof(buf), f)) {
        char *p = buf;
        while (*p && *p != ':') p++;
        if (*p) p++;
        while (*p && *p != ':') p++;
        if (*p) p++;
        int uid = atoi(p);
        if (uid > max && uid < 60000) max = uid;
    }
    fclose(f);
    return max + 1;
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        putstr("useradd: only root can add users\n");
        return 1;
    }
    if (argc < 2) {
        putstr("usage: useradd [-m] [-s shell] <username>\n");
        return 1;
    }

    const char *shell = "/bin/ksh";
    int make_home = 0;
    const char *user = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0)
            make_home = 1;
        else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
            shell = argv[++i];
        else if (argv[i][0] != '-')
            user = argv[i];
    }
    if (!user) {
        putstr("useradd: no username given\n");
        return 1;
    }
    if (getpwnam(user)) {
        putstr("useradd: user already exists\n");
        return 1;
    }

    int uid = next_uid();
    char home[128];
    snprintf(home, sizeof(home), "/home/%s", user);

    FILE *f = fopen("/etc/passwd", "a");
    if (!f) {
        putstr("useradd: cannot open /etc/passwd\n");
        return 1;
    }
    fprintf(f, "%s:x:%d:%d:%s:%s:%s\n", user, uid, uid, user, home, shell);
    fclose(f);

    f = fopen("/etc/shadow", "a");
    if (f) {
        fprintf(f, "%s:!:20000:0:99999:7:::\n", user);
        fclose(f);
    }

    f = fopen("/etc/group", "a");
    if (f) {
        fprintf(f, "%s:x:%d:\n", user, uid);
        fclose(f);
    }

    if (make_home) {
        mkdir("/home", 0755);
        mkdir(home, 0755);
        chown(home, uid, uid);
    }

    putstr("useradd: user created\n");
    return 0;
}
