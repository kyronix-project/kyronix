#define _GNU_SOURCE
#include "common.h"
#include <crypt.h>
#include <shadow.h>
#include <time.h>

static void usage(void) {
    fprintf(stderr,
        "usage: useradd [-m] [-c comment] [-d home] [-g group]\n"
        "               [-G groups] [-s shell] [-u uid] [-p password] login\n");
    exit(1);
}

static int next_uid(void) {
    int uid = 1000;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return uid;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        p = strchr(p + 1, ':');
        if (!p) continue;
        int id = atoi(p + 1);
        if (id >= uid) uid = id + 1;
    }
    fclose(f);
    return uid;
}

static int next_gid(void) {
    int gid = 1000;
    FILE *f = fopen("/etc/group", "r");
    if (!f) return gid;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        p = strchr(p + 1, ':');
        if (!p) continue;
        int id = atoi(p + 1);
        if (id >= gid) gid = id + 1;
    }
    fclose(f);
    return gid;
}

static int resolve_gid(const char *name) {
    FILE *fp = fopen("/etc/group", "r");
    if (!fp) return -1;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char gname[64] = "", pass[64] = "", gid_s[32] = "";
        sscanf(line, "%63[^:]:%63[^:]:%31[^:]", gname, pass, gid_s);
        if (strcmp(gname, name) == 0) {
            fclose(fp);
            return atoi(gid_s);
        }
    }
    fclose(fp);
    return -1;
}

static void add_to_group(const char *gname, const char *user) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/etc/group.tmp");
    FILE *fin = fopen("/etc/group", "r");
    FILE *fout = fopen(tmp, "w");
    if (!fin || !fout) { if (fin) fclose(fin); if (fout) fclose(fout); return; }
    char line[512];
    while (fgets(line, sizeof(line), fin)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char name[64] = "", pass[64] = "", gid_s[32] = "", members[256] = "";
        sscanf(line, "%63[^:]:%63[^:]:%31[^:]:%255s", name, pass, gid_s, members);
        if (strcmp(name, gname) == 0) {
            if (strstr(members, user)) {
                fprintf(fout, "%s\n", line);
            } else {
                if (*members)
                    fprintf(fout, "%s:x:%s:%s,%s\n", name, gid_s, members, user);
                else
                    fprintf(fout, "%s:x:%s:%s\n", name, gid_s, user);
            }
        } else {
            fprintf(fout, "%s\n", line);
        }
    }
    fclose(fin);
    fflush(fout);
    fsync(fileno(fout));
    fclose(fout);
    rename(tmp, "/etc/group");
}

int main(int argc, char **argv) {
    kx_prog = "useradd";

    const char *comment = "";
    const char *home = NULL;
    const char *shell = "/bin/ksh";
    const char *password = NULL;
    const char *supgroups = NULL;
    int create_home = 0;
    int uid = -1;
    int gid = -1;
    int has_gid = 0;

    int i;
    const char *login = NULL;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-m") == 0) create_home = 1;
            else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) comment = argv[++i];
            else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) home = argv[++i];
            else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) shell = argv[++i];
            else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) uid = atoi(argv[++i]);
            else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
                i++;
                has_gid = 1;
                int g = resolve_gid(argv[i]);
                if (g >= 0) gid = g;
                else gid = atoi(argv[i]);
            }
            else if (strcmp(argv[i], "-G") == 0 && i + 1 < argc) supgroups = argv[++i];
            else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) password = argv[++i];
            else usage();
        } else {
            login = argv[i];
        }
    }

    if (!login) usage();

    {
        FILE *check = fopen("/etc/passwd", "r");
        if (!check) { kx_warn("/etc/passwd"); exit(1); }
        char line[512];
        while (fgets(line, sizeof(line), check)) {
            char *p = strchr(line, ':');
            if (p && (size_t)(p - line) == strlen(login) && memcmp(line, login, strlen(login)) == 0) {
                fprintf(stderr, "useradd: user '%s' already exists\n", login);
                fclose(check);
                return 1;
            }
        }
        fclose(check);
    }

    if (uid < 0) uid = next_uid();
    if (gid < 0) gid = next_gid();

    if (!home) {
        static char defhome[PATH_MAX];
        snprintf(defhome, sizeof(defhome), "/home/%s", login);
        home = defhome;
    }

    FILE *fp = fopen("/etc/passwd", "a");
    if (!fp) { kx_warn("/etc/passwd"); exit(1); }
    fprintf(fp, "%s:x:%d:%d:%s:%s:%s\n", login, uid, gid, comment, home, shell);
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);

    FILE *fs = fopen("/etc/shadow", "a");
    if (!fs) { kx_warn("/etc/shadow"); exit(1); }
    long today = (long)(time(NULL) / 86400);
    if (password) {
        char salt[64];
        snprintf(salt, sizeof(salt), "$6$%ld$", today);
        const char *hash = crypt(password, salt);
        fprintf(fs, "%s:%s:%ld:0:99999:7:::\n", login, hash ? hash : password, today);
    } else {
        fprintf(fs, "%s::%ld:0:99999:7:::\n", login, today);
    }
    fflush(fs);
    fsync(fileno(fs));
    fclose(fs);

    if (!has_gid) {
        FILE *fg = fopen("/etc/group", "a");
        if (!fg) { kx_warn("/etc/group"); exit(1); }
        fprintf(fg, "%s:x:%d:\n", login, gid);
        fflush(fg);
        fsync(fileno(fg));
        fclose(fg);
    }

    const char *to_add[64];
    int n_to_add = 0;

    if (supgroups) {
        char buf[1024];
        strncpy(buf, supgroups, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *save = NULL;
        char *tok = strtok_r(buf, ",", &save);
        while (tok && n_to_add < 63) {
            to_add[n_to_add++] = tok;
            tok = strtok_r(NULL, ",", &save);
        }
    }

    for (int g = 0; g < n_to_add; g++)
        add_to_group(to_add[g], login);

    if (n_to_add == 0) {
        FILE *fg = fopen("/etc/group", "r");
        if (fg) {
            char line[512];
            while (fgets(line, sizeof(line), fg)) {
                char gname[64] = "", pass[64] = "", gid_s[32] = "";
                sscanf(line, "%63[^:]:%63[^:]:%31[^:]", gname, pass, gid_s);
                if (atoi(gid_s) == gid && strcmp(gname, login) != 0) {
                    add_to_group(gname, login);
                    break;
                }
            }
            fclose(fg);
        }
    }

    if (create_home) {
        int home_ok = (mkdir(home, 0755) == 0 || errno == EEXIST);
        if (!home_ok) {
            mkdir("/home", 0755);
            home_ok = (mkdir(home, 0755) == 0 || errno == EEXIST);
        }
        if (home_ok) {
            char dst[PATH_MAX * 2];
            snprintf(dst, sizeof(dst), "%s/.profile", home);
            FILE *fin = fopen("/etc/skel/.profile", "r");
            if (fin) {
                FILE *fout = fopen(dst, "w");
                if (fout) {
                    char b[512];
                    size_t n;
                    while ((n = fread(b, 1, sizeof(b), fin)) > 0)
                        fwrite(b, 1, n, fout);
                    fflush(fout);
                    fsync(fileno(fout));
                    fclose(fout);
                }
                fclose(fin);
            }
        } else {
            kx_warn(home);
        }
    }

    printf("useradd: user '%s' added (uid=%d, gid=%d)\n", login, uid, gid);
    return 0;
}
