#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "de.h"

#define MAX_ENTRIES 256

static char *strndup_own(const char *s, size_t n)
{
    char *r = malloc(n + 1);
    if (!r)
        return NULL;
    memcpy(r, s, n);
    r[n] = '\0';
    return r;
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        e--;
    *e = '\0';
    return s;
}

static int parse_desktop_file(LauncherEntry **arr, int *n, const char *path)
{
    FILE *fp;
    char line[1024];
    char name[512] = {0}, icon[256] = {0}, exec[512] = {0};
    int in_desktop = 0, has_name = 0;

    fp = fopen(path, "r");
    if (!fp)
        return 0;

    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '\0' || *p == '#')
            continue;
        if (*p == '[') {
            in_desktop = strcmp(p, "[Desktop Entry]") == 0;
            continue;
        }
        if (!in_desktop)
            continue;

        if (strncmp(p, "Name=", 5) == 0) {
            strncpy(name, p + 5, sizeof(name) - 1);
            has_name = 1;
        } else if (strncmp(p, "Icon=", 5) == 0) {
            strncpy(icon, p + 5, sizeof(icon) - 1);
        } else if (strncmp(p, "Exec=", 5) == 0) {
            strncpy(exec, p + 5, sizeof(exec) - 1);
        }

        /* [Desktop Action ...] splits would end the block; only the first
         * [Desktop Entry] section matters. A nested section header exits. */
    }
    fclose(fp);

    if (!has_name || exec[0] == '\0' || *n >= MAX_ENTRIES)
        return 0;

    LauncherEntry *e = calloc(1, sizeof(*e));
    e->name = strndup_own(name, strlen(name));
    e->icon = (icon[0] ? strndup_own(icon, strlen(icon)) : NULL);

    /* Strip any %f/%u/%U/%F field codes from Exec. */
    e->exec = calloc(1, strlen(exec) + 1);
    char *d = e->exec;
    for (char *s = exec; *s; s++) {
        if (*s == '%' && s[1] != '\0' &&
            strchr("fFuUdDnNickvm", s[1])) {
            s++;
            continue;
        }
        *d++ = *s;
    }
    *d = '\0';

    arr[(*n)++] = e;
    return 1;
}

int de_load_dir(LauncherEntry ***out, const char *dir)
{
    LauncherEntry *arr[MAX_ENTRIES];
    int n = 0;
    DIR *dh = opendir(dir);

    if (dh) {
        struct dirent *de;
        while ((de = readdir(dh)) != NULL && n < MAX_ENTRIES) {
            size_t len = strlen(de->d_name);
            if (len < 9 || strcmp(de->d_name + len - 8, ".desktop") != 0)
                continue;
            char *path = malloc(strlen(dir) + len + 2);
            sprintf(path, "%s/%s", dir, de->d_name);
            parse_desktop_file(arr, &n, path);
            free(path);
        }
        closedir(dh);
    }

    LauncherEntry **res = calloc(n + 1, sizeof(*res));
    for (int i = 0; i < n; i++)
        res[i] = arr[i];
    *out = res;
    return n;
}

void launcher_entry_free(LauncherEntry *e)
{
    if (!e)
        return;
    free(e->name);
    free(e->icon);
    free(e->exec);
    free(e);
}

LauncherEntry *launcher_entry_clone(const LauncherEntry *e)
{
    LauncherEntry *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->name = strndup_own(e->name ? e->name : "", strlen(e->name ? e->name : ""));
    c->icon = (e->icon ? strndup_own(e->icon, strlen(e->icon)) : NULL);
    c->exec = strndup_own(e->exec ? e->exec : "", strlen(e->exec ? e->exec : ""));
    return c;
}
