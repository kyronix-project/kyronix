#define _GNU_SOURCE
#include "common.h"
#include <grp.h>
#include <shadow.h>

static void usage(void) {
    fprintf(stderr, "usage: userdel [-r] login\n");
    exit(1);
}

static int remove_from_file(const char *path, const char *key, int field) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fin = fopen(path, "r");
    if (!fin) { kx_warn(path); return 1; }
    FILE *fout = fopen(tmp, "w");
    if (!fout) { fclose(fin); kx_warn(tmp); return 1; }

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), fin)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char copy[512];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *save = NULL;
        char *tok = strtok_r(copy, ":", &save);
        int f = 0;
        while (tok && f < field) { tok = strtok_r(NULL, ":", &save); f++; }

        if (tok && strcmp(tok, key) == 0) {
            found = 1;
            continue;
        }
        fprintf(fout, "%s\n", line);
    }

    fclose(fin);
    fflush(fout);
    fsync(fileno(fout));
    fclose(fout);
    rename(tmp, path);
    return found ? 0 : 1;
}

static int remove_from_group_members(const char *login) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "/etc/group.tmp");

    FILE *fin = fopen("/etc/group", "r");
    if (!fin) return 1;
    FILE *fout = fopen(tmp, "w");
    if (!fout) { fclose(fin); return 1; }

    char line[512];
    int modified = 0;
    while (fgets(line, sizeof(line), fin)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char copy[512];
        strncpy(copy, line, sizeof(copy) - 1);
        copy[sizeof(copy) - 1] = '\0';

        char *name = strtok(copy, ":");
        char *pass = strtok(NULL, ":");
        char *gid_s = strtok(NULL, ":");
        char *members = strtok(NULL, "");

        if (name && members && *members) {
            char newmembers[256] = "";
            int first = 1;
            char mcopy[256];
            strncpy(mcopy, members, sizeof(mcopy) - 1);
            mcopy[sizeof(mcopy) - 1] = '\0';
            char *save = NULL;
            char *tok = strtok_r(mcopy, ",", &save);
            while (tok) {
                if (strcmp(tok, login) != 0) {
                    if (!first) strcat(newmembers, ",");
                    strcat(newmembers, tok);
                    first = 0;
                } else {
                    modified = 1;
                }
                tok = strtok_r(NULL, ",", &save);
            }
            fprintf(fout, "%s:%s:%s:%s\n", name, pass ? pass : "x",
                    gid_s ? gid_s : "0", newmembers);
        } else {
            fprintf(fout, "%s\n", line);
        }
    }

    fclose(fin);
    fflush(fout);
    fsync(fileno(fout));
    fclose(fout);
    rename(tmp, "/etc/group");
    return modified;
}

static int remove_dir(const char *path) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    return system(cmd);
}

static char *get_home_dir(const char *login) {
    struct passwd *pw = getpwnam(login);
    return pw ? strdup(pw->pw_dir) : NULL;
}

int main(int argc, char **argv) {
    kx_prog = "userdel";

    int remove_home = 0;
    int i = 1;

    for (; i < argc && argv[i][0] == '-'; i++) {
        if (strcmp(argv[i], "-r") == 0) remove_home = 1;
        else usage();
    }

    if (i >= argc) usage();
    const char *login = argv[i];

    if (strcmp(login, "root") == 0) {
        fprintf(stderr, "userdel: refusing to remove root\n");
        return 1;
    }

    if (!getpwnam(login)) {
        fprintf(stderr, "userdel: user '%s' not found\n", login);
        return 1;
    }

    char *homedir = get_home_dir(login);

    remove_from_file("/etc/passwd", login, 0);
    remove_from_file("/etc/shadow", login, 0);

    struct group *gr;
    setgrent();
    while ((gr = getgrent())) {
        if (gr->gr_gid != 0 && strcmp(gr->gr_name, login) == 0)
            remove_from_file("/etc/group", login, 0);
    }
    endgrent();

    remove_from_group_members(login);

    if (remove_home && homedir && homedir[0] == '/')
        remove_dir(homedir);

    free(homedir);
    printf("userdel: user '%s' removed\n", login);
    return 0;
}
